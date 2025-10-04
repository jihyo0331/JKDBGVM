#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/statvfs.h>

#include "time.h"

#define READ_BUFSZ 65536
#define WRITE_TIMEOUT_MS 2000
#define READ_TIMEOUT_MS  8000
#define MAX_RETRY_COUNT 5
#define QMP_HANDSHAKE_RETRY 3
#define RETRY_BACKOFF_MS 200

static const char *g_sock_path = NULL;
static const char *g_timelog_path = NULL;
static char g_snapshot_dir[PATH_MAX];
static char g_disk_image_path[PATH_MAX];
static char g_detected_socket[PATH_MAX];
static bool g_warned_missing_socket = false;

#define DISK_SLICE_DEFAULT_MB   100ULL
#define MEM_SLICE_DEFAULT_MB    256ULL

static uint64_t g_disk_slice_bytes = DISK_SLICE_DEFAULT_MB * 1024ULL * 1024ULL;
static uint64_t g_mem_slice_bytes  = MEM_SLICE_DEFAULT_MB * 1024ULL * 1024ULL;

typedef struct SnapshotArtifacts {
    char disk_bz_path[PATH_MAX];
    uint64_t disk_bytes;          /* raw bytes copied from disk */
    uint64_t disk_archive_bytes;  /* compressed artifact size */
    char mem_bz_path[PATH_MAX];
    uint64_t mem_bytes;           /* raw bytes dumped from memory */
    uint64_t mem_archive_bytes;   /* compressed artifact size */
} SnapshotArtifacts;

typedef struct VmPauseGuard {
    bool acquired;
    bool need_resume;
} VmPauseGuard;

static int qmp_stop_vm(void);
static int qmp_resume_vm(void);
static VmPauseGuard vm_pause_for_operation(const char *label);
static void vm_resume_after_operation(const char *label, VmPauseGuard guard);

static bool looks_like_qmp_error(const char *resp);
static char *qmp_exec_simple(const char *json);
static int restore_snapshot_artifacts(const char *snap_name);
static int delete_snapshot_internal(const char *name);
static int delete_snapshot(const char *name);
static bool qmp_is_running(void);

static uint64_t parse_env_bytes(const char *env_name, uint64_t default_bytes) {
    const char *val = getenv(env_name);
    if (!val || !*val) {
        return default_bytes;
    }

    char *endptr = NULL;
    double mb = strtod(val, &endptr);
    if (endptr == val || mb <= 0.0) {
        timing_log(LOG_WARN, "%s 환경 변수를 해석할 수 없어 기본값 사용", env_name);
        return default_bytes;
    }
    return (uint64_t)(mb * 1024.0 * 1024.0);
}

static void init_snapshot_paths(void) {
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = ".";
    }

    const char *disk_env = getenv("SNAPCTL_DISK_IMAGE");
    if (disk_env && *disk_env) {
        snprintf(g_disk_image_path, sizeof(g_disk_image_path), "%s", disk_env);
    } else {
        snprintf(g_disk_image_path, sizeof(g_disk_image_path), "%s/vm/win11/disk.qcow2", home);
    }

    const char *dir_env = getenv("SNAPCTL_ARCHIVE_DIR");
    if (dir_env && *dir_env) {
        snprintf(g_snapshot_dir, sizeof(g_snapshot_dir), "%s", dir_env);
    } else {
        snprintf(g_snapshot_dir, sizeof(g_snapshot_dir), "%s/vm/win11/snapshots", home);
    }

    g_disk_slice_bytes = parse_env_bytes("SNAPCTL_DISK_SLICE_MB", g_disk_slice_bytes);
    g_mem_slice_bytes  = parse_env_bytes("SNAPCTL_MEM_SLICE_MB",  g_mem_slice_bytes);
}

static bool path_is_socket_file(const char *path) {
    if (!path || !*path) {
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISSOCK(st.st_mode);
}

static bool find_socket_recursive(const char *dir, int depth, const char *target_name,
                                  bool allow_any, char *out, size_t out_sz) {
    if (!dir || depth < 0) {
        return false;
    }

    DIR *dp = opendir(dir);
    if (!dp) {
        return false;
    }

    struct dirent *ent;
    bool found = false;
    while ((ent = readdir(dp)) != NULL && !found) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", dir, name) <= 0) {
            continue;
        }

        struct stat sb;
        if (lstat(path, &sb) != 0) {
            continue;
        }

        if (S_ISSOCK(sb.st_mode)) {
            if (target_name && strcmp(name, target_name) == 0) {
                strncpy(out, path, out_sz);
                out[out_sz - 1] = '\0';
                found = true;
            } else if (!target_name && allow_any) {
                strncpy(out, path, out_sz);
                out[out_sz - 1] = '\0';
                found = true;
            }
        } else if (S_ISDIR(sb.st_mode) && depth > 0) {
            if (find_socket_recursive(path, depth - 1, target_name, allow_any, out, out_sz)) {
                found = true;
            }
        }
    }

    closedir(dp);
    return found;
}

static const char *auto_detect_socket(void) {
    if (g_detected_socket[0]) {
        return g_detected_socket;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) {
        return NULL;
    }

    char base[PATH_MAX];
    if (snprintf(base, sizeof(base), "%s/vm", home) <= 0) {
        return NULL;
    }

    if (path_is_socket_file(base)) {
        strncpy(g_detected_socket, base, sizeof(g_detected_socket));
        g_detected_socket[sizeof(g_detected_socket) - 1] = '\0';
        return g_detected_socket;
    }

    if (find_socket_recursive(base, 4, "qmp.sock", false, g_detected_socket, sizeof(g_detected_socket))) {
        return g_detected_socket;
    }

    if (find_socket_recursive(base, 4, NULL, true, g_detected_socket, sizeof(g_detected_socket))) {
        return g_detected_socket;
    }

    return NULL;
}

static int ensure_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        timing_log(LOG_ERROR, "경로가 디렉터리가 아닙니다: %s", path);
        return -1;
    }
    if (errno != ENOENT) {
        timing_log(LOG_ERROR, "stat(%s) 실패: %s", path, strerror(errno));
        return -1;
    }
    if (mkdir(path, 0775) == 0) {
        timing_log(LOG_INFO, "스냅샷 아카이브 디렉터리를 생성했습니다: %s", path);
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    timing_log(LOG_ERROR, "mkdir(%s) 실패: %s", path, strerror(errno));
    return -1;
}

static int copy_file_slice(const char *src, const char *dst, uint64_t length) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        timing_log(LOG_ERROR, "원본 파일을 열 수 없습니다 (%s): %s", src, strerror(errno));
        return -1;
    }

    FILE *out = fopen(dst, "wb");
    if (!out) {
        timing_log(LOG_ERROR, "출력 파일을 열 수 없습니다 (%s): %s", dst, strerror(errno));
        close(sfd);
        return -1;
    }

    const size_t buf_sz = 1 * 1024 * 1024; // 1 MiB
    uint8_t *buffer = malloc(buf_sz);
    if (!buffer) {
        timing_log(LOG_ERROR, "버퍼 할당 실패");
        fclose(out);
        close(sfd);
        return -1;
    }

    ssize_t r;
    uint64_t remaining = length;
    while (remaining > 0) {
        size_t to_read = remaining < buf_sz ? (size_t)remaining : buf_sz;
        r = read(sfd, buffer, to_read);
        if (r < 0) {
            if (errno == EINTR) continue;
            timing_log(LOG_ERROR, "파일 읽기 실패 (%s): %s", src, strerror(errno));
            free(buffer);
            fclose(out);
            close(sfd);
            return -1;
        }
        if (r == 0) {
            break; // EOF
        }
        if (fwrite(buffer, 1, (size_t)r, out) != (size_t)r) {
            timing_log(LOG_ERROR, "파일 쓰기 실패 (%s): %s", dst, strerror(errno));
            free(buffer);
            fclose(out);
            close(sfd);
            return -1;
        }
        remaining -= (uint64_t)r;
    }

    free(buffer);
    fclose(out);
    close(sfd);
    return 0;
}

static void escape_quotes(const char *src, char *dst, size_t dst_sz) {
    size_t pos = 0;
    for (const char *p = src; *p && pos + 1 < dst_sz; ++p) {
        if (*p == '"') {
            if (pos + 2 >= dst_sz) {
                break;
            }
            dst[pos++] = '\\';
        }
        dst[pos++] = *p;
    }
    dst[pos] = '\0';
}

static int run_process(const char *const argv[], const char *label) {
    if (!argv || !argv[0]) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        timing_log(LOG_ERROR, "%s 실행을 위한 fork 실패: %s", label ? label : argv[0], strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        timing_log(LOG_ERROR, "%s waitpid 실패: %s", label ? label : argv[0], strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    if (WIFEXITED(status)) {
        timing_log(LOG_ERROR, "%s 실패 (exit status=%d)",
                   label ? label : argv[0], WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        timing_log(LOG_ERROR, "%s가 시그널(%d)로 종료되었습니다",
                   label ? label : argv[0], WTERMSIG(status));
    } else {
        timing_log(LOG_ERROR, "%s 실패 (status=0x%x)",
                   label ? label : argv[0], status);
    }

    return -1;
}

static int run_bzip2_filter(const char *src_path, const char *dest_path, bool decompress) {
    if (!src_path || !dest_path) {
        return -1;
    }

    int in_fd = open(src_path, O_RDONLY);
    if (in_fd < 0) {
        timing_log(LOG_ERROR, "bzip2 입력 파일을 열 수 없습니다 (%s): %s", src_path, strerror(errno));
        return -1;
    }

    int out_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        timing_log(LOG_ERROR, "bzip2 출력 파일을 열 수 없습니다 (%s): %s", dest_path, strerror(errno));
        close(in_fd);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        timing_log(LOG_ERROR, "bzip2 처리를 위한 fork 실패: %s", strerror(errno));
        close(in_fd);
        close(out_fd);
        return -1;
    }

    if (pid == 0) {
        if (dup2(in_fd, STDIN_FILENO) < 0) {
            _exit(126);
        }
        if (dup2(out_fd, STDOUT_FILENO) < 0) {
            _exit(126);
        }

        close(in_fd);
        close(out_fd);

        if (decompress) {
            execlp("bzip2", "bzip2", "-d", "-c", (char *)NULL);
        } else {
            execlp("bzip2", "bzip2", "-f", "-c", (char *)NULL);
        }
        _exit(127);
    }

    close(in_fd);
    close(out_fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        timing_log(LOG_ERROR, "bzip2 처리 waitpid 실패: %s", strerror(errno));
        return -1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }

    if (WIFEXITED(status)) {
        timing_log(LOG_ERROR, "bzip2 %s 실패 (exit status=%d)",
                   decompress ? "해제" : "압축", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        timing_log(LOG_ERROR, "bzip2 %s가 시그널(%d)로 종료되었습니다",
                   decompress ? "해제" : "압축", WTERMSIG(status));
    } else {
        timing_log(LOG_ERROR, "bzip2 %s 실패 (status=0x%x)",
                   decompress ? "해제" : "압축", status);
    }

    return -1;
}

static int compress_with_bzip2(const char *src_path, const char *dest_path) {
    return run_bzip2_filter(src_path, dest_path, false);
}

static int decompress_with_bzip2(const char *src_path, const char *dest_path) {
    int rc = run_bzip2_filter(src_path, dest_path, true);
    if (rc < 0) {
        unlink(dest_path);
    }
    return rc;
}

static void remove_snapshot_artifacts(const char *snap_name) {
    if (!snap_name || !*snap_name) {
        return;
    }

    const char *suffixes[] = {
        "-disk.bz2",
        "-mem.bz2",
        "-mem.raw",
        "-disk.raw",
        "-fast-meta.json",
        "-fast-snapshot.tar.bz2",
        "-fast.ready.lock",
    };

    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        int written = snprintf(path, sizeof(path), "%s/%s%s", g_snapshot_dir, snap_name, suffixes[i]);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }
        if (unlink(path) == 0) {
            timing_log(LOG_INFO, "스냅샷 보조 파일을 삭제했습니다: %s", path);
        } else if (errno != ENOENT) {
            timing_log(LOG_WARN, "스냅샷 보조 파일 삭제 실패 (%s): %s", path, strerror(errno));
        }
    }
}

static int compact_disk_image(void) {
    if (!g_disk_image_path[0]) {
        timing_log(LOG_ERROR, "디스크 이미지 경로가 초기화되지 않았습니다");
        return -1;
    }

    if (qmp_is_running()) {
        timing_log(LOG_ERROR, "VM이 실행 중이어서 디스크를 압축할 수 없습니다. VM을 완전히 종료한 뒤 다시 시도하세요");
        return -1;
    }

    struct stat st;
    if (stat(g_disk_image_path, &st) != 0) {
        timing_log(LOG_ERROR, "디스크 이미지를 확인할 수 없습니다 (%s): %s", g_disk_image_path, strerror(errno));
        return -1;
    }

    char dir[PATH_MAX];
    char base[PATH_MAX];
    const char *slash = strrchr(g_disk_image_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - g_disk_image_path);
        if (dir_len == 0) {
            snprintf(dir, sizeof(dir), "/");
        } else {
            snprintf(dir, sizeof(dir), "%.*s", (int)dir_len, g_disk_image_path);
        }
        snprintf(base, sizeof(base), "%s", slash + 1);
    } else {
        snprintf(dir, sizeof(dir), ".");
        snprintf(base, sizeof(base), "%s", g_disk_image_path);
    }

    struct statvfs svfs;
    if (statvfs(dir, &svfs) != 0) {
        timing_log(LOG_WARN, "디스크 여유 공간을 확인할 수 없습니다 (%s): %s", dir, strerror(errno));
    } else {
        unsigned long long avail = (unsigned long long)svfs.f_bavail * (unsigned long long)svfs.f_frsize;
        unsigned long long required = (unsigned long long)st.st_size + (100ULL * 1024ULL * 1024ULL);
        if (avail < required) {
            timing_log(LOG_ERROR, "디스크 공간이 부족합니다 (필요: %.1f MB, 사용 가능: %.1f MB). 불필요한 파일을 정리한 뒤 다시 시도하세요",
                       required / (1024.0 * 1024.0), avail / (1024.0 * 1024.0));
            return -1;
        }
    }

    char tmp_path[PATH_MAX];
    int written = snprintf(tmp_path, sizeof(tmp_path), "%s/.snapctl-compact-%s-XXXXXX", dir, base);
    if (written < 0 || (size_t)written >= sizeof(tmp_path)) {
        timing_log(LOG_ERROR, "디스크 압축 임시 경로가 너무 깁니다");
        return -1;
    }

    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        timing_log(LOG_ERROR, "디스크 압축 임시 파일 생성 실패 (%s): %s", tmp_path, strerror(errno));
        return -1;
    }
    close(tmp_fd);
    if (unlink(tmp_path) != 0) {
        if (errno != ENOENT) {
            timing_log(LOG_WARN, "임시 파일을 삭제하지 못했습니다 (%s): %s", tmp_path, strerror(errno));
        }
    }

    const char *convert_argv[] = {
        "qemu-img", "convert", "-p", "-O", "qcow2", g_disk_image_path, tmp_path, NULL
    };

    TimingContext *ctx = timing_start("compact disk image");
    int rc = run_process(convert_argv, "qemu-img convert");
    double elapsed = ctx ? timing_end(ctx) : 0.0;

    if (rc != 0) {
        unlink(tmp_path);
        return -1;
    }

    char backup_path[PATH_MAX];
    pid_t pid = getpid();
    int attempt = 0;
    int backup_rc = -1;
    backup_path[0] = '\0';
    while (attempt < 5) {
        written = snprintf(backup_path, sizeof(backup_path), "%s/.snapctl-compact-backup-%d-%d", dir, (int)pid, attempt);
        if (written < 0 || (size_t)written >= sizeof(backup_path)) {
            break;
        }
        if (access(backup_path, F_OK) != 0) {
            backup_rc = 0;
            break;
        }
        attempt++;
    }

    if (backup_rc != 0) {
        timing_log(LOG_ERROR, "백업 경로를 준비할 수 없습니다: %s", backup_path);
        unlink(tmp_path);
        return -1;
    }

    if (rename(g_disk_image_path, backup_path) != 0) {
        timing_log(LOG_ERROR, "원본 디스크를 백업으로 이동할 수 없습니다 (%s -> %s): %s",
                   g_disk_image_path, backup_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    if (rename(tmp_path, g_disk_image_path) != 0) {
        timing_log(LOG_ERROR, "압축 디스크를 최종 경로로 이동할 수 없습니다 (%s -> %s): %s",
                   tmp_path, g_disk_image_path, strerror(errno));
        int restore_rc = rename(backup_path, g_disk_image_path);
        if (restore_rc != 0) {
            timing_log(LOG_ERROR, "백업 복원 실패 (%s -> %s): %s", backup_path, g_disk_image_path, strerror(errno));
        }
        unlink(tmp_path);
        return -1;
    }

    if (unlink(backup_path) != 0) {
        timing_log(LOG_WARN, "백업 파일을 삭제하지 못했습니다 (%s): %s", backup_path, strerror(errno));
    }

    struct stat new_st;
    if (stat(g_disk_image_path, &new_st) == 0) {
        timing_log(LOG_INFO, "디스크 이미지를 압축했습니다 (%.1f MB -> %.1f MB, %.2f s)",
                   st.st_size / (1024.0 * 1024.0),
                   new_st.st_size / (1024.0 * 1024.0),
                   elapsed);
    } else {
        int err = errno;
        timing_log(LOG_WARN, "압축 후 디스크 크기를 확인할 수 없습니다 (%s): %s",
                   g_disk_image_path, strerror(err));
        timing_log(LOG_INFO, "디스크 이미지를 압축했습니다 (%.1f MB, %.2f s, 새 크기 확인 실패)",
                   st.st_size / (1024.0 * 1024.0), elapsed);
    }

    return 0;
}
static void path_basename_copy(const char *path, char *dst, size_t dst_sz) {
    if (!dst || dst_sz == 0) {
        return;
    }

    const char *base = strrchr(path, '/');
    if (base) {
        base++; /* skip '/' */
    } else {
        base = path;
    }

    if (!base) {
        dst[0] = '\0';
        return;
    }

    snprintf(dst, dst_sz, "%s", base);
}

static int create_fast_snapshot_bundle(const char *snap_name, const SnapshotArtifacts *artifacts) {
    if (!snap_name || !artifacts) {
        return -1;
    }

    char meta_path[PATH_MAX];
    int written = snprintf(meta_path, sizeof(meta_path), "%s/%s-fast-meta.json", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(meta_path)) {
        timing_log(LOG_ERROR, "fast snapshot 메타 경로가 너무 깁니다");
        return -1;
    }

    char disk_base[PATH_MAX];
    char mem_base[PATH_MAX];
    char meta_base[PATH_MAX];
    path_basename_copy(artifacts->disk_bz_path, disk_base, sizeof(disk_base));
    path_basename_copy(artifacts->mem_bz_path, mem_base, sizeof(mem_base));
    path_basename_copy(meta_path, meta_base, sizeof(meta_base));

    FILE *meta = fopen(meta_path, "w");
    if (!meta) {
        timing_log(LOG_ERROR, "fast snapshot 메타 파일을 생성할 수 없습니다 (%s): %s", meta_path, strerror(errno));
        return -1;
    }

    time_t now = time(NULL);
    fprintf(meta,
            "{\n"
            "  \"snapshot\": \"%s\",\n"
            "  \"created_at\": %lld,\n"
            "  \"disk_bytes\": %llu,\n"
            "  \"disk_archive_bytes\": %llu,\n"
            "  \"disk_file\": \"%s\",\n"
            "  \"mem_bytes\": %llu,\n"
            "  \"mem_archive_bytes\": %llu,\n"
            "  \"mem_file\": \"%s\"\n"
            "}\n",
            snap_name,
            (long long)now,
            (unsigned long long)artifacts->disk_bytes,
            (unsigned long long)artifacts->disk_archive_bytes,
            disk_base,
            (unsigned long long)artifacts->mem_bytes,
            (unsigned long long)artifacts->mem_archive_bytes,
            mem_base);
    fclose(meta);

    char bundle_path[PATH_MAX];
    written = snprintf(bundle_path, sizeof(bundle_path), "%s/%s-fast-snapshot.tar.bz2", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(bundle_path)) {
        timing_log(LOG_ERROR, "fast snapshot 번들 경로가 너무 깁니다");
        unlink(meta_path);
        return -1;
    }

    char escaped_bundle[PATH_MAX * 2];
    char escaped_dir[PATH_MAX * 2];
    char escaped_disk[PATH_MAX * 2];
    char escaped_mem[PATH_MAX * 2];
    char escaped_meta[PATH_MAX * 2];

    escape_quotes(bundle_path, escaped_bundle, sizeof(escaped_bundle));
    escape_quotes(g_snapshot_dir, escaped_dir, sizeof(escaped_dir));
    escape_quotes(disk_base, escaped_disk, sizeof(escaped_disk));
    escape_quotes(mem_base, escaped_mem, sizeof(escaped_mem));
    escape_quotes(meta_base, escaped_meta, sizeof(escaped_meta));

    char cmd[PATH_MAX * 8];
    written = snprintf(cmd, sizeof(cmd),
                       "tar -cjf \"%s\" -C \"%s\" \"%s\" -C \"%s\" \"%s\" -C \"%s\" \"%s\"",
                       escaped_bundle, escaped_dir, escaped_disk,
                       escaped_dir, escaped_mem,
                       escaped_dir, escaped_meta);
    if (written < 0 || (size_t)written >= sizeof(cmd)) {
        timing_log(LOG_ERROR, "fast snapshot tar 명령이 너무 깁니다");
        unlink(meta_path);
        return -1;
    }

    int rc = system(cmd);
    if (rc != 0) {
        timing_log(LOG_ERROR, "fast snapshot tar 생성 실패 (명령=%s, 코드=%d)", cmd, rc);
        unlink(bundle_path);
        unlink(meta_path);
        return -1;
    }

    char ready_path[PATH_MAX];
    written = snprintf(ready_path, sizeof(ready_path), "%s/%s-fast.ready.lock", g_snapshot_dir, snap_name);
    if (written >= 0 && (size_t)written < sizeof(ready_path)) {
        int fd = open(ready_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            timing_log(LOG_WARN, "fast snapshot ready 파일 생성 실패 (%s): %s", ready_path, strerror(errno));
        } else {
            close(fd);
        }
    }

    timing_log(LOG_INFO, "fast snapshot 번들을 생성했습니다: %s", bundle_path);
    return 0;
}

static int archive_disk_slice(const char *snap_name, SnapshotArtifacts *artifacts) {
    if (ensure_directory(g_snapshot_dir) < 0) {
        return -1;
    }

    struct stat st;
    if (stat(g_disk_image_path, &st) < 0) {
        timing_log(LOG_ERROR, "디스크 이미지가 없습니다 (%s): %s", g_disk_image_path, strerror(errno));
        return -1;
    }

    uint64_t slice = g_disk_slice_bytes;
    if ((uint64_t)st.st_size < slice) {
        slice = (uint64_t)st.st_size;
    }
    if (slice == 0) {
        timing_log(LOG_WARN, "디스크 이미지 크기가 0입니다, 압축을 건너뜁니다");
        return -1;
    }

    char raw_path[PATH_MAX];
    char bz_path[PATH_MAX];
    int written = snprintf(raw_path, sizeof(raw_path), "%s/%s-disk.raw", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(raw_path)) {
        timing_log(LOG_ERROR, "디스크 스냅샷 경로가 너무 깁니다");
        return -1;
    }
    written = snprintf(bz_path, sizeof(bz_path), "%s/%s-disk.bz2", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(bz_path)) {
        timing_log(LOG_ERROR, "디스크 압축 경로가 너무 깁니다");
        return -1;
    }

    TimingContext *ctx = timing_start("partial disk copy");
    if (copy_file_slice(g_disk_image_path, raw_path, slice) < 0) {
        timing_log(LOG_ERROR, "디스크 부분 복사 실패 (%s)", snap_name);
        if (ctx) {
            timing_end(ctx);
        }
        unlink(raw_path);
        return -1;
    }
    if (ctx) {
        timing_end(ctx);
    }

    ctx = timing_start("partial disk compress");
    int rc = compress_with_bzip2(raw_path, bz_path);
    double comp_s = ctx ? timing_end(ctx) : 0.0;

    unlink(raw_path);
    if (rc < 0) {
        unlink(bz_path);
        return -1;
    }

    timing_log(LOG_INFO, "디스크 부분 스냅샷 압축 완료: %s (%.1f MB, %.2f s)",
               bz_path, slice / (1024.0 * 1024.0), comp_s);

    if (artifacts) {
        strncpy(artifacts->disk_bz_path, bz_path, sizeof(artifacts->disk_bz_path));
        artifacts->disk_bz_path[sizeof(artifacts->disk_bz_path) - 1] = '\0';
        artifacts->disk_bytes = slice;
        artifacts->disk_archive_bytes = 0;

        struct stat bz_st;
        if (stat(bz_path, &bz_st) == 0) {
            artifacts->disk_archive_bytes = (uint64_t)bz_st.st_size;
        }
    }
    return 0;
}

static int dump_memory_slice(const char *snap_name, char *raw_out, size_t raw_out_sz) {
    int written = snprintf(raw_out, raw_out_sz, "%s/%s-mem.raw", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= raw_out_sz) {
        timing_log(LOG_ERROR, "메모리 덤프 경로가 너무 깁니다");
        return -1;
    }
    unlink(raw_out);

    char json[2048];
    char escaped_path[PATH_MAX * 2];
    escape_quotes(raw_out, escaped_path, sizeof(escaped_path));
    written = snprintf(json, sizeof(json),
             "{\"execute\":\"dump-guest-memory\",\"arguments\":{"
             "\"paging\":false,\"protocol\":\"file:%s\",\"begin\":0,\"length\":%llu}}",
             escaped_path, (unsigned long long)g_mem_slice_bytes);
    if (written < 0 || (size_t)written >= sizeof(json)) {
        timing_log(LOG_ERROR, "dump-guest-memory JSON이 너무 깁니다");
        unlink(raw_out);
        return -1;
    }

    TimingContext *ctx = timing_start("dump guest memory slice");
    char *resp = qmp_exec_simple(json);
    double dump_s = ctx ? timing_end(ctx) : 0.0;

    if (!resp || looks_like_qmp_error(resp)) {
        timing_log(LOG_ERROR, "dump-guest-memory 실패: %s", resp ? resp : "(no response)");
        free(resp);
        unlink(raw_out);
        return -1;
    }
    free(resp);

    timing_log(LOG_INFO, "메모리 부분 덤프 완료 (%.1f MB, %.2f s): %s",
               g_mem_slice_bytes / (1024.0 * 1024.0), dump_s, raw_out);
    return 0;
}

static int archive_memory_slice(const char *snap_name, SnapshotArtifacts *artifacts) {
    if (ensure_directory(g_snapshot_dir) < 0) {
        return -1;
    }

    char raw_path[PATH_MAX];
    if (dump_memory_slice(snap_name, raw_path, sizeof(raw_path)) < 0) {
        return -1;
    }

    uint64_t raw_bytes = g_mem_slice_bytes;
    struct stat raw_st;
    if (stat(raw_path, &raw_st) == 0) {
        raw_bytes = (uint64_t)raw_st.st_size;
    }

    char bz_path[PATH_MAX];
    int written = snprintf(bz_path, sizeof(bz_path), "%s/%s-mem.bz2", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(bz_path)) {
        timing_log(LOG_ERROR, "메모리 압축 경로가 너무 깁니다");
        unlink(raw_path);
        return -1;
    }

    TimingContext *ctx = timing_start("compress memory slice");
    int rc = compress_with_bzip2(raw_path, bz_path);
    double comp_s = ctx ? timing_end(ctx) : 0.0;
    unlink(raw_path);
    if (rc < 0) {
        unlink(bz_path);
        return -1;
    }

    timing_log(LOG_INFO, "메모리 부분 스냅샷 압축 완료: %s (%.2f s)", bz_path, comp_s);

    if (artifacts) {
        strncpy(artifacts->mem_bz_path, bz_path, sizeof(artifacts->mem_bz_path));
        artifacts->mem_bz_path[sizeof(artifacts->mem_bz_path) - 1] = '\0';
        artifacts->mem_bytes = raw_bytes;
        artifacts->mem_archive_bytes = 0;

        struct stat bz_st;
        if (stat(bz_path, &bz_st) == 0) {
            artifacts->mem_archive_bytes = (uint64_t)bz_st.st_size;
        }
    }
    return 0;
}

static void archive_snapshot_artifacts(const char *snap_name) {
    TimingContext *ctx = timing_start("snapctl archive");
    SnapshotArtifacts artifacts;
    memset(&artifacts, 0, sizeof(artifacts));

    int disk_rc = archive_disk_slice(snap_name, &artifacts);
    int mem_rc  = archive_memory_slice(snap_name, &artifacts);
    double total_s = ctx ? timing_end(ctx) : 0.0;

    if (disk_rc == 0 && mem_rc == 0) {
        int fast_rc = create_fast_snapshot_bundle(snap_name, &artifacts);
        if (fast_rc == 0) {
            timing_log(LOG_INFO, "스냅샷 보조 아카이브 및 fast snapshot 번들 완료: %s (%.2f s)",
                       snap_name, total_s);
        } else {
            timing_log(LOG_WARN, "fast snapshot 번들 생성에 실패했습니다: %s", snap_name);
        }
    } else {
        timing_log(LOG_ERROR, "스냅샷 보조 아카이브 실패: %s (%.2f s) (disk=%d, mem=%d)",
                   snap_name, total_s, disk_rc, mem_rc);
    }
}

static int apply_disk_slice_from_file(const char *raw_path, uint64_t slice_bytes) {
    int raw_fd = open(raw_path, O_RDONLY);
    if (raw_fd < 0) {
        timing_log(LOG_ERROR, "디스크 슬라이스 파일을 열 수 없습니다 (%s): %s", raw_path, strerror(errno));
        return -1;
    }

    int disk_fd = open(g_disk_image_path, O_WRONLY);
    if (disk_fd < 0) {
        timing_log(LOG_ERROR, "디스크 이미지를 열 수 없습니다 (%s): %s", g_disk_image_path, strerror(errno));
        close(raw_fd);
        return -1;
    }

    const size_t buf_sz = 1 * 1024 * 1024;
    uint8_t *buffer = malloc(buf_sz);
    if (!buffer) {
        timing_log(LOG_ERROR, "디스크 복원 버퍼 할당 실패");
        close(raw_fd);
        close(disk_fd);
        return -1;
    }

    uint64_t remaining = slice_bytes;
    off_t offset = 0;
    while (remaining > 0) {
        size_t chunk = remaining < buf_sz ? (size_t)remaining : buf_sz;
        ssize_t r = read(raw_fd, buffer, chunk);
        if (r < 0) {
            if (errno == EINTR) continue;
            timing_log(LOG_ERROR, "디스크 슬라이스 읽기 실패 (%s): %s", raw_path, strerror(errno));
            free(buffer);
            close(raw_fd);
            close(disk_fd);
            return -1;
        }
        if (r == 0) {
            break;
        }
        ssize_t w = pwrite(disk_fd, buffer, (size_t)r, offset);
        if (w != r) {
            timing_log(LOG_ERROR, "디스크 이미지 쓰기 실패 (%s): %s", g_disk_image_path, strerror(errno));
            free(buffer);
            close(raw_fd);
            close(disk_fd);
            return -1;
        }
        offset += r;
        remaining -= (uint64_t)r;
    }

    free(buffer);
    close(raw_fd);
    close(disk_fd);
    return 0;
}

static int restore_disk_slice(const char *snap_name) {
    int written;
    char src_path[PATH_MAX];
    written = snprintf(src_path, sizeof(src_path), "%s/%s-disk.bz2", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(src_path)) {
        timing_log(LOG_ERROR, "디스크 보조 스냅샷 경로가 너무 깁니다");
        return -1;
    }

    if (access(src_path, R_OK) != 0) {
        timing_log(LOG_WARN, "디스크 보조 스냅샷이 없습니다: %s", src_path);
        return -1;
    }

    char tmp_path[] = "/tmp/snapctl-diskXXXXXX";
    int tmp_fd = mkstemp(tmp_path);
    if (tmp_fd < 0) {
        timing_log(LOG_ERROR, "디스크 임시 파일 생성 실패: %s", strerror(errno));
        return -1;
    }
    close(tmp_fd);

    if (decompress_with_bzip2(src_path, tmp_path) < 0) {
        unlink(tmp_path);
        return -1;
    }

    struct stat st;
    if (stat(tmp_path, &st) < 0) {
        timing_log(LOG_ERROR, "디스크 슬라이스 크기를 확인할 수 없습니다 (%s): %s", tmp_path, strerror(errno));
        unlink(tmp_path);
        return -1;
    }

    TimingContext *ctx = timing_start("restore disk slice");
    int rc = apply_disk_slice_from_file(tmp_path, (uint64_t)st.st_size);
    double elapsed = ctx ? timing_end(ctx) : 0.0;
    unlink(tmp_path);

    if (rc == 0) {
        timing_log(LOG_INFO, "디스크 보조 스냅샷을 복원했습니다 (%.1f MB, %.2f s)",
                   st.st_size / (1024.0 * 1024.0), elapsed);
    }
    return rc;
}

static int restore_memory_slice(const char *snap_name) {
    int written;
    char src_path[PATH_MAX];
    written = snprintf(src_path, sizeof(src_path), "%s/%s-mem.bz2", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(src_path)) {
        timing_log(LOG_ERROR, "메모리 보조 스냅샷 경로가 너무 깁니다");
        return -1;
    }

    if (access(src_path, R_OK) != 0) {
        timing_log(LOG_WARN, "메모리 보조 스냅샷이 없습니다: %s", src_path);
        return -1;
    }

    char raw_path[PATH_MAX];
    written = snprintf(raw_path, sizeof(raw_path), "%s/%s-mem.raw", g_snapshot_dir, snap_name);
    if (written < 0 || (size_t)written >= sizeof(raw_path)) {
        timing_log(LOG_ERROR, "메모리 보조 스냅샷 복원 경로가 너무 깁니다");
        return -1;
    }

    if (decompress_with_bzip2(src_path, raw_path) < 0) {
        return -1;
    }

    struct stat st;
    if (stat(raw_path, &st) < 0) {
        timing_log(LOG_ERROR, "메모리 슬라이스 크기를 확인할 수 없습니다 (%s): %s", raw_path, strerror(errno));
        unlink(raw_path);
        return -1;
    }

    timing_log(LOG_INFO, "메모리 보조 스냅샷을 해제했습니다 (%.1f MB): %s",
               st.st_size / (1024.0 * 1024.0), raw_path);
    timing_log(LOG_INFO, "주의: dump-guest-memory 결과는 참조용이며 loadvm이 메모리를 복원합니다.");
    // 메모리 파일은 사용자가 필요 시 활용하도록 남겨둠.
    return 0;
}

static int restore_snapshot_artifacts(const char *snap_name) {
    int disk_rc = restore_disk_slice(snap_name);
    int mem_rc  = restore_memory_slice(snap_name);

    if (disk_rc == 0 && mem_rc == 0) {
        timing_log(LOG_INFO, "보조 스냅샷 복원 완료: %s", snap_name);
        return 0;
    }

    timing_log(LOG_WARN, "보조 스냅샷 복원 중 일부 실패: %s (disk=%d, mem=%d)",
               snap_name, disk_rc, mem_rc);
    return -1;
}
/* ---------- util ---------- */
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static int set_timeouts(int fd, int r_ms, int w_ms) {
    struct timeval r = { .tv_sec = r_ms/1000, .tv_usec = (r_ms%1000)*1000 };
    struct timeval w = { .tv_sec = w_ms/1000, .tv_usec = (w_ms%1000)*1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w)) < 0) return -1;
    return 0;
}

static void sleep_ms(unsigned int ms) {
    struct timespec req = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        continue;
    }
}

// 버퍼링된 읽기로 성능 개선
static char *read_line_buffered(int fd) {
    static char buffer[READ_BUFSZ];
    static char *buf_ptr = NULL;
    static size_t buf_len = 0;
    static char line_buf[READ_BUFSZ];
    
    size_t line_pos = 0;
    
    while (line_pos < sizeof(line_buf) - 1) {
        // 버퍼가 비어있으면 새로 읽기
        if (buf_len == 0) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                if (n == 0 && line_pos > 0) break;
                if (n < 0 && errno == EINTR) continue;
                return NULL;
            }
            buf_ptr = buffer;
            buf_len = n;
        }
        
        // 버퍼에서 한 문자씩 처리
        char c = *buf_ptr++;
        buf_len--;
        line_buf[line_pos++] = c;
        
        if (c == '\n') break;
    }
    
    line_buf[line_pos] = '\0';
    return line_pos > 0 ? strdup(line_buf) : NULL;
}

static char *read_resp_line(int fd) {
    for (;;) {
        char *line = read_line_buffered(fd);
        if (!line) return NULL;
        // 이벤트 메시지는 건너뛰기
        if (strstr(line, "\"event\"")) { 
            free(line); 
            continue; 
        }
        return line;
    }
}

static int send_line(int fd, const char *json) {
    size_t len = strlen(json);
    // 한 번에 전송하도록 개선
    char *buf = malloc(len + 2);
    if (!buf) return -1;
    
    memcpy(buf, json, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    
    ssize_t sent = send(fd, buf, len + 1, 0);
    free(buf);
    
    return (sent == (ssize_t)(len + 1)) ? 0 : -1;
}

static bool looks_like_qmp_error(const char *resp) {
    if (!resp) return true;
    
    // 명시적인 에러만 에러로 판단
    if (strstr(resp, "\"error\"") != NULL) return true;
    if (strstr(resp, "GenericError") != NULL) return true;
    if (strstr(resp, "CommandNotFound") != NULL) return true;
    
    // 빈 응답이나 성공 응답은 에러가 아님
    return false;
}

/* ---------- QMP 연결 & 네고 ---------- */
static int qmp_connect(const char *sockpath) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        timing_log(LOG_ERROR, "socket(%s) 실패: %s", sockpath, strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        timing_log(LOG_ERROR, "fcntl(F_GETFL) 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        timing_log(LOG_ERROR, "fcntl(F_SETFL, O_NONBLOCK) 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sockpath) >= sizeof(addr.sun_path)) {
        timing_log(LOG_ERROR, "소켓 경로가 너무 깁니다: %s", sockpath);
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, sockpath);

    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        if (errno == ENOENT || errno == ECONNREFUSED) {
            timing_log(LOG_DEBUG, "connect(%s) 실패: %s", sockpath, strerror(errno));
        } else {
            timing_log(LOG_ERROR, "connect(%s) 실패: %s", sockpath, strerror(errno));
        }
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags) < 0) {
        timing_log(LOG_ERROR, "fcntl(F_SETFL, blocking) 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (set_timeouts(fd, READ_TIMEOUT_MS, WRITE_TIMEOUT_MS) < 0) {
        timing_log(LOG_ERROR, "setsockopt timeouts 실패: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int qmp_open_and_negotiate(void) {
    bool missing_socket = false;
    bool refused = false;
    for (int attempt = 1; attempt <= QMP_HANDSHAKE_RETRY; ++attempt) {
        int fd = qmp_connect(g_sock_path);
        if (fd < 0) {
            if (errno == ENOENT) {
                missing_socket = true;
                if (!g_warned_missing_socket) {
                    timing_log(LOG_WARN, "QMP 소켓을 찾을 수 없어 QMP 기능을 비활성화합니다: %s", g_sock_path);
                    g_warned_missing_socket = true;
                }
                break;
            }
            if (errno == ECONNREFUSED) {
                refused = true;
                break;
            }
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }

        errno = 0;
        char *banner = read_line_buffered(fd);
        if (!banner) {
            timing_log(errno == EAGAIN || errno == EWOULDBLOCK ? LOG_WARN : LOG_ERROR,
                       "QMP 배너를 읽지 못했습니다 (시도 %d/%d)", attempt, QMP_HANDSHAKE_RETRY);
            close(fd);
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }
        free(banner);

        if (send_line(fd, "{\"execute\":\"qmp_capabilities\"}") < 0) {
            timing_log(LOG_ERROR, "qmp_capabilities 전송 실패: %s", strerror(errno));
            close(fd);
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }

        errno = 0;
        char *neg = read_resp_line(fd);
        if (!neg) {
            timing_log(errno == EAGAIN || errno == EWOULDBLOCK ? LOG_WARN : LOG_ERROR,
                       "QMP capabilities 응답을 읽지 못했습니다 (시도 %d/%d)",
                       attempt, QMP_HANDSHAKE_RETRY);
            close(fd);
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }
        free(neg);
        g_warned_missing_socket = false;
        return fd;
    }

    if (missing_socket) {
        return -1;
    }
    if (refused) {
        timing_log(LOG_WARN, "QMP 소켓은 존재하지만 연결을 거부했습니다. VM이 QMP 옵션으로 실행 중인지 확인하세요: %s", g_sock_path);
        return -1;
    }

    timing_log(LOG_ERROR, "QMP handshake 실패: 소켓 %s", g_sock_path);
    return -1;
}

/* ---------- QMP/HMP 명령 ---------- */
static char *qmp_exec_simple(const char *json) {
    int fd = qmp_open_and_negotiate();
    if (fd < 0) {
        return NULL;
    }
    if (send_line(fd, json) < 0) {
        timing_log(LOG_ERROR, "QMP 명령 전송 실패: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    errno = 0;
    char *resp = read_resp_line(fd);
    if (!resp && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
        timing_log(LOG_ERROR, "QMP 응답을 읽지 못했습니다");
    }
    close(fd);
    return resp;
}

static char *qmp_hmp_passthru(const char *hmp) {
    int fd = qmp_open_and_negotiate();
    if (fd < 0) {
        return NULL;
    }
    char *resp = NULL;
    char *payload = NULL;
    
    if (asprintf(&payload,
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}",
                 hmp) < 0) payload = NULL;
    if (!payload) { close(fd); return NULL; }

    if (send_line(fd, payload) == 0) {
        errno = 0;
        resp = read_resp_line(fd);
    } else {
        timing_log(LOG_ERROR, "HMP 명령 전송 실패: %s", strerror(errno));
    }
    if (!resp && !(errno == EAGAIN || errno == EWOULDBLOCK)) {
        timing_log(LOG_ERROR, "HMP 응답을 읽지 못했습니다");
    }
    free(payload);
    close(fd);
    return resp;
}

static int qmp_run_command(const char *json, const char *hmp, const char *label, const char *nonfatal_substr) {
    int rc = -1;
    char *resp = qmp_exec_simple(json);
    if (resp) {
        if (!looks_like_qmp_error(resp)) {
            rc = 0;
        } else if (nonfatal_substr && strstr(resp, nonfatal_substr)) {
            rc = 0;
        } else if (label) {
            timing_log(LOG_WARN, "%s QMP 응답: %s", label, resp);
        }
        free(resp);
    }

    if (rc == 0) {
        return 0;
    }

    if (hmp) {
        resp = qmp_hmp_passthru(hmp);
        bool ok = resp && strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL;
        if (!ok && nonfatal_substr && resp && strstr(resp, nonfatal_substr)) {
            ok = true;
        }
        if (!ok && label) {
            timing_log(LOG_WARN, "%s HMP 응답: %s", label, resp ? resp : "(null)");
        }
        if (resp) {
            free(resp);
        }
        if (ok) {
            rc = 0;
        }
    }

    return rc;
}

static int qmp_stop_vm(void) {
    return qmp_run_command("{\"execute\":\"stop\"}", "stop", "stop", "already");
}

static int qmp_resume_vm(void) {
    return qmp_run_command("{\"execute\":\"cont\"}", "cont", "cont", "already");
}

/* ---------- 상태 확인 및 resume 보정 (개선됨) ---------- */
static bool looks_running(const char *resp) {
    if (!resp) return false;
    return strstr(resp, "\"status\":\"running\"") || strstr(resp, "\"running\":true");
}

static bool snapshot_exists(const char *name) {
    if (!name || !*name) {
        return false;
    }

    char *resp = qmp_exec_simple("{\"execute\":\"query-savevm\"}");
    if (resp) {
        bool found = strstr(resp, name) != NULL;
        free(resp);
        if (found) {
            return true;
        }
    }

    resp = qmp_hmp_passthru("info snapshots");
    if (!resp) {
        return false;
    }
    bool found = strstr(resp, name) != NULL;
    free(resp);
    return found;
}

static bool vm_running_hmp(void) {
    char *resp = qmp_hmp_passthru("info status");
    if (!resp) {
        timing_log(LOG_WARN, "VM 실행 상태를 확인하지 못했습니다 (HMP)");
        return false;
    }
    bool running = strstr(resp, "running") != NULL;
    free(resp);
    return running;
}

static bool qmp_is_running(void) {
    char *resp = qmp_exec_simple("{\"execute\":\"query-status\"}");
    if (!resp) {
        return false;
    }

    if (looks_like_qmp_error(resp)) {
        bool unsupported = strstr(resp, "\"CommandNotFound\"") != NULL;
        free(resp);
        return unsupported && vm_running_hmp();
    }

    bool ok = looks_running(resp);
    free(resp);
    return ok;
}

static VmPauseGuard vm_pause_for_operation(const char *label) {
    VmPauseGuard guard = { .acquired = false, .need_resume = false };

    bool was_running = qmp_is_running();
    int rc = qmp_stop_vm();
    if (rc == 0) {
        guard.acquired = true;
        guard.need_resume = was_running;
        if (was_running) {
            timing_log(LOG_INFO, "VM을 일시정지했습니다 (%s)", label ? label : "operation");
        }
    } else {
        timing_log(LOG_WARN, "VM 일시정지에 실패했습니다 (%s)", label ? label : "operation");
    }

    return guard;
}

static void vm_resume_after_operation(const char *label, VmPauseGuard guard) {
    if (!guard.acquired || !guard.need_resume) {
        return;
    }

    if (qmp_resume_vm() == 0) {
        timing_log(LOG_INFO, "VM을 다시 실행했습니다 (%s)", label ? label : "operation");
    } else {
        timing_log(LOG_ERROR, "VM 재실행에 실패했습니다 (%s)", label ? label : "operation");
    }
}

// 더 적극적인 resume 로직으로 paused 문제 해결
static int qmp_ensure_running(void) {
    for (int i = 0; i < MAX_RETRY_COUNT; i++) {
        if (qmp_is_running()) return 0;
        
        // 점진적으로 대기 시간 증가
        struct timespec ts = { 
            .tv_sec = 0, 
            .tv_nsec = (50 + i * 50) * 1000 * 1000  // 50ms, 100ms, 150ms...
        };
        nanosleep(&ts, NULL);
        
        // cont 명령 여러 번 시도
        for (int j = 0; j < 3; j++) {
            qmp_run_command("{\"execute\":\"cont\"}", "cont", "cont(retry)", "already");

            struct timespec short_wait = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&short_wait, NULL);
        }
        
        // 최종 확인
        if (qmp_is_running()) return 0;
    }

    fprintf(stderr, "Warning: VM may still be paused after %d attempts\n", MAX_RETRY_COUNT);
    return -1;
}

/* ---------- 스냅샷 API (개선됨) ---------- */
static int save_snapshot(const char *name) {
    timing_log(LOG_DEBUG, "Starting savevm for snapshot: %s", name);
    VmPauseGuard guard = vm_pause_for_operation("savevm");
    bool success = false;
    int rc = -1;

    if (snapshot_exists(name)) {
        timing_log(LOG_WARN, "Snapshot '%s' already exists. 기존 스냅샷을 삭제합니다.", name);
        if (delete_snapshot_internal(name) != 0) {
            timing_log(LOG_ERROR, "기존 스냅샷 '%s' 삭제 실패", name);
            goto out;
        }
        remove_snapshot_artifacts(name);
    }

    // QMP 우선 시도
    char json[1024];
    snprintf(json, sizeof(json), "{\"execute\":\"savevm\",\"arguments\":{\"name\":\"%s\"}}", name);
    char *resp = qmp_exec_simple(json);
    
    // QMP 응답 분석 (빈 응답도 성공으로 간주)
    if (resp) {
        if (!looks_like_qmp_error(resp)) {
            success = true;
        } else if (strstr(resp, "\"CommandNotFound\"")) {
            timing_log(LOG_INFO, "savevm QMP 명령이 지원되지 않아 HMP 경로로 폴백합니다");
        } else {
            timing_log(LOG_ERROR, "savevm QMP 응답: %s", resp);
        }
        free(resp);
    }
    
    // QMP가 실패했다면 HMP 폴백
    if (!success) {
        char hmp[1024];
        snprintf(hmp, sizeof(hmp), "savevm %s", name);
        resp = qmp_hmp_passthru(hmp);
        if (resp) {
            success = (strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL);
            if (!success) {
                timing_log(LOG_ERROR, "savevm HMP 응답: %s", resp);
            }
            free(resp);
        }
    }
    
    // 실제로 스냅샷이 생성되었는지 최종 확인
    if (!success) {
        struct timespec wait_time = { .tv_sec = 0, .tv_nsec = 500 * 1000 * 1000 }; // 500ms
        nanosleep(&wait_time, NULL);

        if (snapshot_exists(name)) {
            success = true;
            timing_log(LOG_INFO, "Snapshot '%s' verified in snapshot list", name);
        }
    }

    rc = success ? 0 : -1;

out:
    vm_resume_after_operation("savevm", guard);
    return rc;
}

static int load_snapshot_internal(const char *name) {
    bool success = false;

    // QMP 시도
    char json[1024];
    snprintf(json, sizeof(json), "{\"execute\":\"loadvm\",\"arguments\":{\"name\":\"%s\"}}", name);
    char *resp = qmp_exec_simple(json);
    if (resp) {
        if (!looks_like_qmp_error(resp)) {
            success = true;
        } else if (strstr(resp, "\"CommandNotFound\"")) {
            timing_log(LOG_INFO, "loadvm QMP 명령이 지원되지 않아 HMP 경로로 폴백합니다");
        } else {
            timing_log(LOG_WARN, "loadvm QMP 응답: %s", resp);
        }
        free(resp);
    }

    // HMP 폴백
    if (!success) {
        char hmp[1024];
        snprintf(hmp, sizeof(hmp), "loadvm %s", name);
        resp = qmp_hmp_passthru(hmp);
        if (resp) {
            success = (strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL);
            if (!success) {
                timing_log(LOG_WARN, "loadvm HMP 응답: %s", resp);
            }
            free(resp);
        }
    }

    if (!success) {
        return -1;
    }

    if (restore_snapshot_artifacts(name) != 0) {
        timing_log(LOG_WARN, "보조 스냅샷 복원 중 문제가 발생했습니다");
    }

    return 0;
}

static int load_snapshot(const char *name) {
    VmPauseGuard guard = vm_pause_for_operation("loadvm");
    int rc = load_snapshot_internal(name);
    vm_resume_after_operation("loadvm", guard);

    if (guard.acquired && guard.need_resume) {
        int resume_rc = qmp_ensure_running();
        if (resume_rc != 0 && rc == 0) {
            timing_log(LOG_WARN, "loadvm 후 VM을 재시작하지 못했습니다");
            rc = -1;
        }
    }

    return rc;
}

static int delete_snapshot_internal(const char *name) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"execute\":\"delvm\",\"arguments\":{\"name\":\"%s\"}}", name);
    char *resp = qmp_exec_simple(json);
    
    if (resp && !looks_like_qmp_error(resp)) { 
        free(resp); 
        return 0; 
    }
    free(resp);
    
    char hmp[1024];
    snprintf(hmp, sizeof(hmp), "delvm %s", name);
    resp = qmp_hmp_passthru(hmp);
    bool ok = (resp && strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL);
    free(resp);
    return ok ? 0 : -1;
}

static int delete_snapshot(const char *name) {
    VmPauseGuard guard = vm_pause_for_operation("delvm");
    int rc = delete_snapshot_internal(name);
    vm_resume_after_operation("delvm", guard);
    return rc;
}

static int list_snapshots(void) {
    char *resp = qmp_exec_simple("{\"execute\":\"query-savevm\"}");
    if (resp && !looks_like_qmp_error(resp)) {
        printf("%s", resp);
        free(resp);
        return 0;
    }
    free(resp);
    
    resp = qmp_hmp_passthru("info snapshots");
    if (!resp) return -1;
    printf("%s", resp);
    free(resp);
    return 0;
}

/* ---------- CLI ---------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --socket <path> <cmd> [name]\n"
        "       %s [--timelog <path>] --socket <path> <cmd> [name]\n"
        "  cmds:\n"
        "    savevm <name>\n"
        "    loadvm <name>\n"
        "    delvm  <name>\n"
        "    deletevm <name>  (스냅샷 삭제 + 보조 파일 정리 + 디스크 압축)\n"
        "    pause            (VM 일시정지)\n"
        "    run              (VM 실행 재개)\n"
        "    compact         (디스크 압축만 수행)\n"
        "    list\n"
        "  options:\n"
        "    --socket <path>    QMP 소켓 경로(기본: $HOME/vm/win11/qmp.sock)\n"
        "    --timelog <path>   스냅샷 작업 시간 로그 파일(기본: ./snapctl-timing.log)\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *socket_arg = NULL;
    const char *timelog_arg = NULL;
    const char *socket_env = getenv("SNAPCTL_SOCKET");
    int i = 1;
    
    while (i < argc) {
        if (!strcmp(argv[i], "--socket") && i+1 < argc) {
            socket_arg = argv[++i];
        } else if (!strcmp(argv[i], "--timelog") && i+1 < argc) {
            timelog_arg = argv[++i];
        } else break;
        i++;
    }

    if (socket_arg && *socket_arg) {
        g_sock_path = socket_arg;
        timing_log(LOG_INFO, "QMP 소켓(명령행): %s", g_sock_path);
    } else if (socket_env && *socket_env) {
        g_sock_path = socket_env;
        timing_log(LOG_INFO, "QMP 소켓(환경변수 SNAPCTL_SOCKET): %s", g_sock_path);
    } else {
        const char *home = getenv("HOME");
        static char def[512];
        if (!home) die("HOME not set and --socket not provided");
        snprintf(def, sizeof(def), "%s/vm/win11/qmp.sock", home);
        if (path_is_socket_file(def)) {
            g_sock_path = def;
            timing_log(LOG_INFO, "QMP 소켓(기본값): %s", g_sock_path);
        } else {
            const char *detected = auto_detect_socket();
            if (detected) {
                g_sock_path = detected;
                timing_log(LOG_INFO, "QMP 소켓 자동 탐지: %s", g_sock_path);
            } else {
                g_sock_path = def;
                timing_log(LOG_WARN,
                           "QMP 소켓을 찾지 못했습니다. 기본 경로를 사용합니다: %s\n"
                           "  - VM을 QMP 옵션과 함께 실행하거나\n"
                           "  - snapctl 실행 시 --socket <path> 를 지정하세요.",
                           g_sock_path);
            }
        }
    }

    if (!path_is_socket_file(g_sock_path)) {
        timing_log(LOG_WARN,
                   "QMP 소켓이 아직 확인되지 않았습니다: %s\n"
                   "  VM을 실행했는지 확인하거나 소켓 경로를 지정하세요.",
                   g_sock_path);
    }

    if (!timelog_arg) {
        timelog_arg = "snapctl-timing.log";
    }

    init_snapshot_paths();
    g_timelog_path = timelog_arg;

    if (timing_init(g_timelog_path) < 0) {
        fprintf(stderr, "Warning: 타이밍 로그 파일을 사용할 수 없습니다. stderr에만 출력합니다.\n");
    } else {
        timing_log(LOG_INFO, "timing log file: %s", g_timelog_path);
    }

    int exit_code = 0;

    if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
    const char *cmd = argv[i++];

    if (!strcmp(cmd, "savevm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *snap_name = argv[i];
        char label[256];
        snprintf(label, sizeof(label), "snapctl savevm:%s", snap_name);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        int rc = save_snapshot(snap_name);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "savevm '%s' %s (%.3f s)",
                   snap_name, rc == 0 ? "성공" : "실패", elapsed_s);

        if (rc == 0) {
            archive_snapshot_artifacts(snap_name);
        }

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "loadvm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *snap_name = argv[i];
        char label[256];
        snprintf(label, sizeof(label), "snapctl loadvm:%s", snap_name);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        int rc = load_snapshot(snap_name);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "loadvm '%s' %s (%.3f s)",
                   snap_name, rc == 0 ? "성공" : "실패", elapsed_s);

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "delvm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *snap_name = argv[i];
        int rc = delete_snapshot(snap_name);
        if (rc == 0) {
            remove_snapshot_artifacts(snap_name);
        }
        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "deletevm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *snap_name = argv[i];
        int rc = delete_snapshot(snap_name);
        if (rc == 0) {
            remove_snapshot_artifacts(snap_name);
            if (!qmp_is_running()) {
                int compact_rc = compact_disk_image();
                if (compact_rc != 0) {
                    timing_log(LOG_WARN,
                               "디스크 압축에 실패했습니다. VM을 완전히 종료한 뒤 'snapctl compact'를 다시 실행하세요.");
                }
            } else {
                timing_log(LOG_INFO,
                           "VM이 실행 중이라 디스크를 압축하지 않았습니다. VM을 종료한 뒤 'snapctl compact'를 실행하세요.");
            }
        }
        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "pause")) {
        if (i < argc) { usage(argv[0]); exit_code = 1; goto out; }
        int rc = qmp_stop_vm();
        if (rc == 0) {
            timing_log(LOG_INFO, "VM을 일시정지했습니다");
        }
        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "run")) {
        if (i < argc) { usage(argv[0]); exit_code = 1; goto out; }
        int rc = qmp_resume_vm();
        if (rc == 0) {
            rc = qmp_ensure_running();
            if (rc == 0) {
                timing_log(LOG_INFO, "VM을 실행 상태로 전환했습니다");
            }
        }
        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "compact")) {
        if (i < argc) { usage(argv[0]); exit_code = 1; goto out; }
        int rc = compact_disk_image();
        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "list")) {
        exit_code = list_snapshots() == 0 ? 0 : 2;
        goto out;
    } else {
        usage(argv[0]);
        exit_code = 1;
        goto out;
    }

out:
    timing_cleanup();
    return exit_code;
}
