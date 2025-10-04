#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>
#include <zlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include "time.h"

#define READ_BUFSZ 65536
#define WRITE_TIMEOUT_MS 2000
#define READ_TIMEOUT_MS  8000
#define MAX_RETRY_COUNT 5
#define QMP_HANDSHAKE_RETRY 3
#define RETRY_BACKOFF_MS 200
#define PIPE_BUFFER_SIZE (1024 * 1024)  // 1MB 파이프 버퍼
#define SNAP_NAME_MAX_LEN 128

static const char *g_sock_path = NULL;
static const char *g_timelog_path = NULL;
static const char *g_snapshot_dir = NULL;
static bool g_block_migration = false;

/* ---------- pigz helper ---------- */
static bool program_exists(const char *prog) {
    if (!prog || !*prog) {
        return false;
    }

    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) {
        return false;
    }

    char *paths = strdup(path_env);
    if (!paths) {
        return false;
    }

    bool found = false;
    char *saveptr = NULL;
    for (char *token = strtok_r(paths, ":", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ":", &saveptr)) {
        if (!*token) {
            continue;
        }
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", token, prog);
        if (access(candidate, X_OK) == 0) {
            found = true;
            break;
        }
    }

    free(paths);
    return found;
}

static bool pigz_available(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = program_exists("pigz") ? 1 : 0;
        if (!cached) {
            timing_log(LOG_DEBUG, "pigz not found in PATH, falling back to zlib");
        }
    }
    return cached != 0;
}

static int pigz_thread_count(void) {
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 1) {
        return 1;
    }
    if (cpu > 64) {
        cpu = 64; // practical upper bound
    }
    return (int)cpu;
}

static char *qmp_cmd(int fd, const char *fmt, ...);
static bool looks_like_qmp_error(const char *resp);
static void sanitize_snapshot_name(const char *name, char *out, size_t out_sz);
static char *hmp_command_raw(int qmp_fd, const char *cmdline);
static int hmp_command_check(int qmp_fd, const char *cmdline);
static bool hmp_response_is_error(const char *resp);
static void hmp_print_return_stdout(const char *resp);

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

/* ---------- Thread-safe 버퍼링된 읽기 ---------- */
typedef struct {
    char buffer[READ_BUFSZ];
    char *buf_ptr;
    size_t buf_len;
} ReadBuffer;

static void init_read_buffer(ReadBuffer *rb) {
    rb->buf_ptr = rb->buffer;
    rb->buf_len = 0;
}

static char *read_line_buffered(int fd, ReadBuffer *rb) {
    char line_buf[READ_BUFSZ];
    size_t line_pos = 0;
    
    while (line_pos < sizeof(line_buf) - 1) {
        // 버퍼가 비어있으면 새로 읽기
        if (rb->buf_len == 0) {
            ssize_t n = recv(fd, rb->buffer, sizeof(rb->buffer), 0);
            if (n <= 0) {
                if (n == 0 && line_pos > 0) break;
                if (n < 0 && errno == EINTR) continue;
                return NULL;
            }
            rb->buf_ptr = rb->buffer;
            rb->buf_len = n;
        }
        
        // 버퍼에서 한 문자씩 처리
        char c = *rb->buf_ptr++;
        rb->buf_len--;
        line_buf[line_pos++] = c;
        
        if (c == '\n') break;
    }
    
    line_buf[line_pos] = '\0';
    return line_pos > 0 ? strdup(line_buf) : NULL;
}

static char *read_resp_line(int fd, ReadBuffer *rb) {
    for (;;) {
        char *line = read_line_buffered(fd, rb);
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
    
    // writev로 한 번에 전송 (메모리 할당 없이)
    struct iovec iov[2];
    iov[0].iov_base = (void*)json;
    iov[0].iov_len = len;
    iov[1].iov_base = (void*)"\n";
    iov[1].iov_len = 1;
    
    ssize_t sent = writev(fd, iov, 2);
    return (sent == (ssize_t)(len + 1)) ? 0 : -1;
}

/* ---------- QMP FD helper ---------- */

static int qmp_getfd(int qmp_fd, int fd_to_send, const char *fdname) {
    char *payload = NULL;
    if (asprintf(&payload,
                 "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"%s\"}}\n",
                 fdname) < 0) {
        return -1;
    }

    struct iovec iov = {
        .iov_base = payload,
        .iov_len = strlen(payload),
    };

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    memset(cmsgbuf, 0, sizeof(cmsgbuf));

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    int ret = sendmsg(qmp_fd, &msg, 0);
    free(payload);
    if (ret < 0) {
        timing_log(LOG_ERROR, "getfd sendmsg 실패: %s", strerror(errno));
        return -1;
    }

    ReadBuffer rb;
    init_read_buffer(&rb);
    char *resp = read_resp_line(qmp_fd, &rb);
    if (!resp) {
        timing_log(LOG_ERROR, "getfd 응답 읽기 실패: %s", strerror(errno));
        return -1;
    }
    bool err = looks_like_qmp_error(resp);
    free(resp);
    if (err) {
        timing_log(LOG_ERROR, "getfd 명령 실패");
        return -1;
    }
    return 0;
}

static int qmp_simple_ok(int qmp_fd, const char *json) {
    char *resp = qmp_cmd(qmp_fd, "%s", json);
    if (!resp) {
        timing_log(LOG_ERROR, "QMP 명령 실패: %s", json);
        return -1;
    }
    bool err = looks_like_qmp_error(resp);
    if (err) {
        timing_log(LOG_ERROR, "QMP 명령 에러(%s): %s", json, resp);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

static int qmp_set_block_migration(int qmp_fd, bool enable) {
    const char *json = enable ?
        "{\"execute\":\"migrate-set-capabilities\",\"arguments\":{\"capabilities\":[{\"capability\":\"block\",\"state\":true}]}}" :
        "{\"execute\":\"migrate-set-capabilities\",\"arguments\":{\"capabilities\":[{\"capability\":\"block\",\"state\":false}]}}";
    char *resp = qmp_cmd(qmp_fd, "%s", json);
    if (!resp) {
        timing_log(LOG_ERROR, "QMP 명령 실패: %s", json);
        return -1;
    }

    if (strstr(resp, "\"error\"")) {
        if (strstr(resp, "does not accept value 'block'") ||
            strstr(resp, "invalid capability") ||
            strstr(resp, "CapabilityNotAvailable")) {
            timing_log(LOG_INFO, "블록 마이그레이션 capability 미지원: %s", resp);
            free(resp);
            return 0;
        }

        timing_log(LOG_ERROR, "QMP 명령 에러(%s): %s", json, resp);
        free(resp);
        return -1;
    }

    free(resp);
    return 0;
}

static const char *qmp_skip_ws(const char *p) {
    while (p && *p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static bool qmp_json_bool(const char *json, const char *key, bool expected) {
    if (!json || !key) {
        return false;
    }

    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return false;
    }

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *cursor = p + strlen(pattern);
        cursor = qmp_skip_ws(cursor);
        if (*cursor != ':') {
            p += 1;
            continue;
        }
        ++cursor;
        cursor = qmp_skip_ws(cursor);

        if (expected && strncmp(cursor, "true", 4) == 0) {
            return true;
        }
        if (!expected && strncmp(cursor, "false", 5) == 0) {
            return true;
        }
        p += 1;
    }
    return false;
}

static bool qmp_json_string(const char *json, const char *key, const char *value) {
    if (!json || !key || !value) {
        return false;
    }

    char pattern[64];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) {
        return false;
    }

    const char *p = json;
    size_t vlen = strlen(value);
    while ((p = strstr(p, pattern)) != NULL) {
        const char *cursor = p + strlen(pattern);
        cursor = qmp_skip_ws(cursor);
        if (*cursor != ':') {
            p += 1;
            continue;
        }
        ++cursor;
        cursor = qmp_skip_ws(cursor);
        if (*cursor != '"') {
            p += 1;
            continue;
        }
        ++cursor;
        if (strncmp(cursor, value, vlen) == 0 && cursor[vlen] == '"') {
            return true;
        }
        p += 1;
    }
    return false;
}

static bool qmp_status_is_running(const char *status_json) {
    if (!status_json) {
        return false;
    }
    return qmp_json_bool(status_json, "running", true) ||
           qmp_json_string(status_json, "status", "running");
}

static bool qmp_status_is_postmigrate(const char *status_json) {
    if (!status_json) {
        return false;
    }
    if (qmp_json_string(status_json, "status", "postmigrate")) {
        return true;
    }
    if (qmp_json_bool(status_json, "postmigrate", true)) {
        return true;
    }
    /* 일부 QEMU 버전은 paused 라벨만 남기는 경우가 있어 같이 검사 */
    if (qmp_json_string(status_json, "status", "paused")) {
        return true;
    }
    return false;
}

static void qmp_try_log_cancel(int qmp_fd, const char *context) {
    char *resp = qmp_cmd(qmp_fd, "{\"execute\":\"migrate-cancel\"}");
    if (!resp) {
        timing_log(LOG_DEBUG, "%s: migrate-cancel 응답 없음", context);
        return;
    }
    if (looks_like_qmp_error(resp)) {
        /* 이미 취소된 경우에는 오류가 올 수 있으므로 디버그 수준으로만 남긴다 */
        timing_log(LOG_DEBUG, "%s: migrate-cancel 결과: %s", context, resp);
    } else {
        timing_log(LOG_INFO, "%s: 이전 마이그레이션을 취소했습니다", context);
    }
    free(resp);
}

static int qmp_ensure_running(int qmp_fd, const char *context) {
    const int max_attempts = 5;

    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        char *status = qmp_cmd(qmp_fd, "{\"execute\":\"query-status\"}");
        if (!status) {
            timing_log(LOG_ERROR, "%s: query-status 실패", context);
            return -1;
        }

        if (qmp_status_is_running(status)) {
            free(status);
            return 0;
        }

        timing_log(LOG_WARN, "%s: VM 상태가 실행 중이 아님 (status=%s)", context, status);
        bool postmigrate = qmp_status_is_postmigrate(status);
        free(status);

        if (postmigrate) {
            qmp_try_log_cancel(qmp_fd, context);
        }

        char *cont = qmp_cmd(qmp_fd, "{\"execute\":\"cont\"}");
        if (!cont) {
            timing_log(LOG_WARN, "%s: cont 명령 응답 없음", context);
        } else {
            if (looks_like_qmp_error(cont)) {
                timing_log(LOG_WARN, "%s: cont 명령 에러: %s", context, cont);
            }
            free(cont);
        }

        struct timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000L };
        nanosleep(&ts, NULL);
    }

    timing_log(LOG_ERROR, "%s: VM을 실행 상태로 복구하지 못했습니다", context);
    return -1;
}

static int ensure_dir_exists(const char *dir) {
    if (!dir || !*dir) {
        return 0;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        return -1;
    }

    if (errno != ENOENT) {
        return -1;
    }

    if (mkdir(dir, 0755) == 0) {
        return 0;
    }
    if (errno == EEXIST) {
        return 0;
    }
    return -1;
}

static char *snapshot_path_from_name(const char *name) {
    if (!name || !*name) {
        return NULL;
    }

    if (strchr(name, '/')) {
        return strdup(name);
    }

    const char *dir = g_snapshot_dir ? g_snapshot_dir : ".";
    size_t len = strlen(name);
    bool has_ext = (len >= 3 && strcmp(name + len - 3, ".gz") == 0);

    char *path = NULL;
    if (has_ext) {
        if (asprintf(&path, "%s/%s", dir, name) < 0) {
            return NULL;
        }
    } else {
        if (asprintf(&path, "%s/%s.gz", dir, name) < 0) {
            return NULL;
        }
    }
    return path;
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

static void sanitize_snapshot_name(const char *name, char *out, size_t out_sz) {
    if (!out || out_sz == 0) {
        return;
    }

    if (!name) {
        name = "";
    }

    size_t pos = 0;
    for (size_t i = 0; name[i] != '\0' && pos < out_sz - 1; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c) || c == '-' || c == '_') {
            out[pos++] = (char)c;
        } else {
            out[pos++] = '_';
        }
    }

    if (pos == 0) {
        const char *fallback = "snap";
        while (*fallback && pos < out_sz - 1) {
            out[pos++] = *fallback++;
        }
    }

    out[pos] = '\0';
}

static char *hmp_command_raw(int qmp_fd, const char *cmdline) {
    if (!cmdline) {
        return NULL;
    }

    char *payload = NULL;
    if (asprintf(&payload,
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}",
                 cmdline) < 0) {
        return NULL;
    }

    char *resp = qmp_cmd(qmp_fd, "%s", payload);
    free(payload);
    return resp;
}

static bool hmp_response_is_error(const char *resp) {
    if (!resp) {
        return true;
    }
    if (looks_like_qmp_error(resp)) {
        return true;
    }
    if (strstr(resp, "Error:") || strstr(resp, "error:")) {
        return true;
    }
    return false;
}

static void hmp_print_return_stdout(const char *resp) {
    if (!resp) {
        return;
    }

    const char *p = strstr(resp, "\"return\"");
    if (!p) {
        return;
    }

    p = strchr(p, ':');
    if (!p) {
        return;
    }
    ++p;

    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '"') {
        return;
    }
    ++p;

    bool printed = false;
    char last = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p) {
                break;
            }
            char c = *p++;
            switch (c) {
            case 'n':
                putchar('\n');
                printed = true;
                last = '\n';
                break;
            case 'r':
                /* 무시 */
                break;
            case 't':
                putchar('\t');
                printed = true;
                last = '\t';
                break;
            case '\\':
                putchar('\\');
                printed = true;
                last = '\\';
                break;
            case '"':
                putchar('"');
                printed = true;
                last = '"';
                break;
            default:
                putchar(c);
                printed = true;
                last = c;
                break;
            }
        } else {
            putchar(*p++);
            printed = true;
            last = *(p - 1);
        }
    }

    if (printed) {
        if (last != '\n') {
            putchar('\n');
        }
    }
}

static int hmp_command_check(int qmp_fd, const char *cmdline) {
    char *resp = hmp_command_raw(qmp_fd, cmdline);
    if (!resp) {
        timing_log(LOG_ERROR, "HMP 명령 실패: %s", cmdline ? cmdline : "(null)");
        return -1;
    }

    if (hmp_response_is_error(resp)) {
        timing_log(LOG_ERROR, "HMP 명령 에러(%s): %s", cmdline, resp);
        free(resp);
        return -1;
    }

    free(resp);
    return 0;
}

static int hmp_save_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) {
        return -1;
    }

    char *cmdline = NULL;
    if (asprintf(&cmdline, "savevm %s", snap_name) < 0) {
        return -1;
    }
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);
    if (rc == 0) {
        timing_log(LOG_INFO, "내부 스냅샷 생성: %s", snap_name);
    }
    return rc;
}

static int hmp_load_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) {
        return -1;
    }

    char *cmdline = NULL;
    if (asprintf(&cmdline, "loadvm %s", snap_name) < 0) {
        return -1;
    }
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);
    if (rc == 0) {
        timing_log(LOG_INFO, "내부 스냅샷 복원: %s", snap_name);
    }
    return rc;
}

static int hmp_delete_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) {
        return -1;
    }

    char *cmdline = NULL;
    if (asprintf(&cmdline, "delvm %s", snap_name) < 0) {
        return -1;
    }
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);
    if (rc == 0) {
        timing_log(LOG_INFO, "내부 스냅샷 삭제: %s", snap_name);
    }
    return rc;
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
        timing_log(LOG_ERROR, "connect(%s) 실패: %s", sockpath, strerror(errno));
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

static int qmp_open_and_negotiate(ReadBuffer *rb) {
    for (int attempt = 1; attempt <= QMP_HANDSHAKE_RETRY; ++attempt) {
        int fd = qmp_connect(g_sock_path);
        if (fd < 0) {
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }

        init_read_buffer(rb);
        
        errno = 0;
        char *banner = read_line_buffered(fd, rb);
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
        char *neg = read_resp_line(fd, rb);
        if (!neg) {
            timing_log(errno == EAGAIN || errno == EWOULDBLOCK ? LOG_WARN : LOG_ERROR,
                       "QMP capabilities 응답을 읽지 못했습니다 (시도 %d/%d)",
                       attempt, QMP_HANDSHAKE_RETRY);
            close(fd);
            sleep_ms(RETRY_BACKOFF_MS * attempt);
            continue;
        }
        free(neg);
        return fd;
    }

    timing_log(LOG_ERROR, "QMP handshake 실패: 소켓 %s", g_sock_path);
    return -1;
}

/* ---------- QMP/HMP 명령 ---------- */
static char *qmp_cmd(int fd, const char *fmt, ...) {
    char *payload = NULL;
    va_list ap; va_start(ap, fmt);
    if (vasprintf(&payload, fmt, ap) < 0) payload = NULL;
    va_end(ap);
    if (!payload) die("OOM");
    
    if (send_line(fd, payload) < 0) {
        timing_log(LOG_ERROR, "QMP 명령 전송 실패: %s", strerror(errno));
        free(payload);
        return NULL;
    }
    free(payload);

    ReadBuffer rb;
    init_read_buffer(&rb);
    
    errno = 0;
    char *resp = read_resp_line(fd, &rb);
    if (!resp) {
        if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
            timing_log(LOG_ERROR, "QMP 응답을 읽지 못했습니다");
        }
    }
    return resp;
}

/* ---------- Migration helpers ---------- */

typedef struct {
    int fd;
    char *path;
    volatile int error;          // atomic 접근
    char errmsg[256];
    pthread_mutex_t mutex;       // 에러 메시지 보호
    volatile int cancel;         // 취소 플래그
    size_t bytes_processed;      // 처리된 바이트 수
} GzipThreadCtx;

static void gzip_thread_set_error(GzipThreadCtx *ctx, const char *fmt, ...) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->error = -1;
    va_list ap; va_start(ap, fmt);
    vsnprintf(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *gzip_source_thread(void *opaque) {
    GzipThreadCtx *ctx = opaque;
    unsigned char buf[READ_BUFSZ];

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    bool use_pigz = pigz_available();
    pid_t pigz_pid = -1;
    int pigz_fd = -1;
    gzFile gz = NULL;

    if (use_pigz) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            timing_log(LOG_WARN, "pigz 파이프 생성 실패: %s", strerror(errno));
            use_pigz = false;
        } else {
            char threads_str[16];
            int threads = pigz_thread_count();
            snprintf(threads_str, sizeof(threads_str), "%d", threads);

            pigz_pid = fork();
            if (pigz_pid == 0) {
                close(pipefd[0]);
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
                    _exit(EXIT_FAILURE);
                }
                close(pipefd[1]);
                execlp("pigz", "pigz", "-d", "-c", "-p", threads_str, ctx->path, (char *)NULL);
                _exit(127);
            } else if (pigz_pid < 0) {
                timing_log(LOG_WARN, "pigz fork 실패: %s", strerror(errno));
                close(pipefd[0]);
                close(pipefd[1]);
                use_pigz = false;
            } else {
                close(pipefd[1]);
                pigz_fd = pipefd[0];
                timing_log(LOG_INFO, "pigz 해제 사용 (%d threads): %s", threads, ctx->path);
            }
        }
    }

    if (!use_pigz) {
        gz = gzopen(ctx->path, "rb");
        if (!gz) {
            gzip_thread_set_error(ctx, "%s: gzip 열기 실패: %s", ctx->path, strerror(errno));
            close(ctx->fd);
            return NULL;
        }
    }

    ctx->bytes_processed = 0;

    while (!ctx->cancel) {
        ssize_t n = 0;

        if (use_pigz) {
            n = read(pigz_fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                gzip_thread_set_error(ctx, "pigz 파이프 읽기 실패: %s", strerror(errno));
                break;
            }
        } else {
            int gn = gzread(gz, buf, sizeof(buf));
            if (gn < 0) {
                int zr;
                const char *msg = gzerror(gz, &zr);
                gzip_thread_set_error(ctx, "gzread 실패: %s", msg);
                break;
            }
            n = gn;
        }

        if (n == 0) {
            timing_log(LOG_INFO, "압축 해제 완료: %.2f MB",
                      ctx->bytes_processed / (1024.0 * 1024.0));
            break;
        }

        ssize_t off = 0;
        while (off < n && !ctx->cancel) {
            ssize_t w = write(ctx->fd, buf + off, (size_t)(n - off));
            if (w > 0) {
                off += w;
            } else if (w < 0 && errno == EINTR) {
                continue;
            } else {
                gzip_thread_set_error(ctx, "pipe 쓰기 실패: %s", strerror(errno));
                if (use_pigz) {
                    if (pigz_fd >= 0) {
                        close(pigz_fd);
                    }
                    if (pigz_pid > 0) {
                        int status;
                        waitpid(pigz_pid, &status, 0);
                    }
                } else {
                    gzclose(gz);
                }
                close(ctx->fd);
                return NULL;
            }
        }

        ctx->bytes_processed += (size_t)n;
    }

    if (use_pigz) {
        if (pigz_fd >= 0) {
            close(pigz_fd);
        }
        if (pigz_pid > 0) {
            int status = 0;
            if (waitpid(pigz_pid, &status, 0) < 0) {
                gzip_thread_set_error(ctx, "pigz 종료 대기 실패: %s", strerror(errno));
            } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                gzip_thread_set_error(ctx, "pigz 종료 코드 %d",
                                       WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
    } else {
        gzclose(gz);
    }

    close(ctx->fd);
    return NULL;
}

typedef struct {
    int pipe_fd;
    int out_fd;
    const char *path;
    volatile int error;
    char errmsg[256];
    pthread_mutex_t mutex;
    volatile int cancel;
    size_t bytes_processed;
} DumpThreadCtx;

static void dump_thread_set_error(DumpThreadCtx *ctx, const char *fmt, ...) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->error = -1;
    va_list ap; va_start(ap, fmt);
    vsnprintf(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&ctx->mutex);
}

static void *raw_dump_thread(void *opaque) {
    DumpThreadCtx *ctx = opaque;
    unsigned char buf[READ_BUFSZ];

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    ctx->bytes_processed = 0;

    while (!ctx->cancel) {
        ssize_t n = read(ctx->pipe_fd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t off = 0;
            while (off < n && !ctx->cancel) {
                ssize_t w = write(ctx->out_fd, buf + off, (size_t)(n - off));
                if (w > 0) {
                    off += w;
                } else if (w < 0 && errno == EINTR) {
                    continue;
                } else {
                    dump_thread_set_error(ctx, "덤프 쓰기 실패: %s", strerror(errno));
                    goto out;
                }
            }
            ctx->bytes_processed += (size_t)n;

        } else if (n == 0) {
            timing_log(LOG_INFO, "덤프 완료: %.2f MB",
                       ctx->bytes_processed / (1024.0 * 1024.0));
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            dump_thread_set_error(ctx, "pipe 읽기 실패: %s", strerror(errno));
            break;
        }
    }

out:
    if (!ctx->error) {
        if (fsync(ctx->out_fd) < 0) {
            dump_thread_set_error(ctx, "fsync 실패: %s", strerror(errno));
        }
    }
    close(ctx->out_fd);
    close(ctx->pipe_fd);
    return NULL;
}

static int compress_raw_snapshot(const char *raw_path, const char *gz_path) {
    int ret = -1;
    int raw_fd = -1;
    gzFile gz = NULL;
    char *tmp_path = NULL;
    unsigned char buf[READ_BUFSZ];

    timing_log(LOG_INFO, "압축 시작: %s", raw_path);

    if (asprintf(&tmp_path, "%s.tmp", gz_path) < 0) {
        timing_log(LOG_ERROR, "임시 파일 경로 생성 실패");
        goto out;
    }

    if (pigz_available()) {
        int threads = pigz_thread_count();
        char threads_str[16];
        snprintf(threads_str, sizeof(threads_str), "%d", threads);

        pid_t pid = fork();
        if (pid == 0) {
            int out_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out_fd < 0) {
                _exit(EXIT_FAILURE);
            }
            if (dup2(out_fd, STDOUT_FILENO) < 0) {
                _exit(EXIT_FAILURE);
            }
            close(out_fd);
            execlp("pigz", "pigz", "-c", "-p", threads_str, raw_path, (char *)NULL);
            _exit(127);
        } else if (pid < 0) {
            timing_log(LOG_WARN, "pigz fork 실패: %s", strerror(errno));
        } else {
            int status = 0;
            if (waitpid(pid, &status, 0) < 0) {
                timing_log(LOG_WARN, "pigz 대기 실패: %s", strerror(errno));
            } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                if (rename(tmp_path, gz_path) < 0) {
                    timing_log(LOG_ERROR, "임시 파일 이동 실패: %s", strerror(errno));
                } else {
                    timing_log(LOG_INFO, "pigz 압축 완료 (%d threads): %s", threads, gz_path);
                    ret = 0;
                    goto out;
                }
            } else {
                timing_log(LOG_WARN, "pigz 종료 코드 %d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            }
        }
        timing_log(LOG_INFO, "pigz 실패로 zlib 경로로 재시도합니다");
        unlink(tmp_path);
    }

    raw_fd = open(raw_path, O_RDONLY);
    if (raw_fd < 0) {
        timing_log(LOG_ERROR, "%s 열기 실패: %s", raw_path, strerror(errno));
        goto out;
    }

    gz = gzopen(tmp_path, "wb");
    if (!gz) {
        timing_log(LOG_ERROR, "%s: gzip 열기 실패", tmp_path);
        goto out;
    }
    if (gzsetparams(gz, 6, Z_DEFAULT_STRATEGY) != Z_OK) {
        timing_log(LOG_ERROR, "gzsetparams 실패: %s", tmp_path);
        goto out;
    }

    size_t total = 0;
    while (1) {
        ssize_t n = read(raw_fd, buf, sizeof(buf));
        if (n > 0) {
            if (gzwrite(gz, buf, (unsigned)n) != n) {
                int zr;
                const char *msg = gzerror(gz, &zr);
                timing_log(LOG_ERROR, "gzwrite 실패 (%zd bytes): %s", n, msg);
                goto out;
            }
            total += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            timing_log(LOG_ERROR, "원본 읽기 실패: %s", strerror(errno));
            goto out;
        }
    }

    if (gzclose(gz) != Z_OK) {
        gz = NULL;
        timing_log(LOG_ERROR, "gzclose 실패");
        goto out;
    }
    gz = NULL;

    if (close(raw_fd) < 0) {
        raw_fd = -1;
        timing_log(LOG_WARN, "원본 파일 닫기 실패: %s", strerror(errno));
    }
    raw_fd = -1;

    if (rename(tmp_path, gz_path) < 0) {
        timing_log(LOG_ERROR, "임시 파일 이동 실패: %s", strerror(errno));
        goto out;
    }

    timing_log(LOG_INFO, "압축 완료: %.2f MB -> %s",
               total / (1024.0 * 1024.0), gz_path);
    ret = 0;

out:
    if (gz) gzclose(gz);
    if (raw_fd >= 0) close(raw_fd);
    if (tmp_path) {
        if (ret != 0) unlink(tmp_path);
        free(tmp_path);
    }
    return ret;
}

static int wait_for_migration_complete(int qfd) {
    // 마이그레이션 중에는 타임아웃을 길게 설정 (1시간)
    struct timeval tv = { .tv_sec = 3600, .tv_usec = 0 };
    setsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    int retry = 0;
    int check_count = 0;
    
    while (1) {
        char *resp = qmp_cmd(qfd, "{\"execute\":\"query-migrate\"}");
        if (!resp) {
            if (++retry > 50) {
                timing_log(LOG_ERROR, "query-migrate 응답 없음 (50회 재시도 실패)");
                return -1;
            }
            sleep_ms(500);
            continue;
        }
        
        retry = 0;
        check_count++;
        
        bool done = qmp_json_string(resp, "status", "completed");
        bool fail = qmp_json_string(resp, "status", "failed") ||
                    qmp_json_string(resp, "status", "cancelled");

        if (!done && !fail && (check_count <= 5 || check_count % 25 == 0)) {
            timing_log(LOG_DEBUG, "query-migrate 응답: %s", resp);
        }

        // 5초마다 진행 상황 로그
        if (check_count % 25 == 0) {
            timing_log(LOG_INFO, "마이그레이션 진행 중... (%d초)", check_count / 5);
        }

        if (done) {
            timing_log(LOG_INFO, "마이그레이션 완료 (응답=%s)", resp);
        } else if (fail) {
            timing_log(LOG_ERROR, "마이그레이션 실패 (응답=%s)", resp);
        }

        free(resp);

        if (done) {
            
            // 타임아웃 원래대로 복구
            struct timeval tv_restore = { 
                .tv_sec = READ_TIMEOUT_MS/1000, 
                .tv_usec = (READ_TIMEOUT_MS%1000)*1000 
            };
            setsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &tv_restore, sizeof(tv_restore));
            
            return 0;
        }
        if (fail) {
            return -1;
        }
        sleep_ms(200);
    }
}

/* ---------- 스냅샷 저장/복원 ---------- */
static int save_snapshot_gz(const char *outfile, const char *hmp_name, bool create_internal) {
    timing_log(LOG_INFO, "savevm-gz 시작: %s", outfile);

    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) {
        return -1;
    }

    int ret = -1;
    bool cont_sent = false;
    bool migration_inflight = false;
    int pipefd[2] = { -1, -1 };
    char *raw_path = NULL;
    int raw_fd = -1;
    DumpThreadCtx ctx;
    bool ctx_initialized = false;
    pthread_t th;
    bool thread_started = false;
    char *resp = NULL;

    if (qmp_ensure_running(qfd, "savevm-gz") < 0) {
        goto cleanup;
    }

    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) {
        goto cleanup;
    }

    if (create_internal && hmp_name && *hmp_name) {
        if (hmp_save_snapshot(qfd, hmp_name) < 0) {
            goto cleanup;
        }
    }

    timing_log(LOG_INFO, "블록 마이그레이션: %s", g_block_migration ? "활성화" : "비활성화");

    if (pipe(pipefd) < 0) {
        timing_log(LOG_ERROR, "pipe 실패: %s", strerror(errno));
        goto cleanup;
    }

    #ifdef F_SETPIPE_SZ
    int pipe_size = PIPE_BUFFER_SIZE;
    if (fcntl(pipefd[0], F_SETPIPE_SZ, pipe_size) < 0) {
        timing_log(LOG_WARN, "파이프 버퍼 크기 설정 실패: %s", strerror(errno));
    } else {
        timing_log(LOG_DEBUG, "파이프 버퍼 크기: %d bytes", pipe_size);
    }
    #endif

    if (asprintf(&raw_path, "%s.rawtmp", outfile) < 0) {
        timing_log(LOG_ERROR, "임시 경로 생성 실패");
        raw_path = NULL;
        goto cleanup;
    }

    raw_fd = open(raw_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (raw_fd < 0) {
        timing_log(LOG_ERROR, "%s 열기 실패: %s", raw_path, strerror(errno));
        goto cleanup;
    }

    ctx = (DumpThreadCtx){
        .pipe_fd = pipefd[0],
        .out_fd = raw_fd,
        .path = raw_path,
        .error = 0,
        .errmsg = {0},
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cancel = 0,
        .bytes_processed = 0,
    };
    ctx_initialized = true;

    if (pthread_create(&th, NULL, raw_dump_thread, &ctx) != 0) {
        timing_log(LOG_ERROR, "덤프 스레드 생성 실패");
        goto cleanup;
    }
    thread_started = true;
    pipefd[0] = -1;  // 스레드가 소유
    raw_fd = -1;

    if (qmp_getfd(qfd, pipefd[1], "snap") < 0) {
        goto cleanup;
    }
    close(pipefd[1]);
    pipefd[1] = -1;

    resp = qmp_cmd(qfd,
        "{\"execute\":\"migrate\",\"arguments\":{\"uri\":\"fd:snap\"}}"
    );
    if (!resp || looks_like_qmp_error(resp)) {
        timing_log(LOG_ERROR, "migrate 명령 실패: %s", resp ? resp : "NULL");
        goto cleanup;
    }
    free(resp);
    resp = NULL;
    migration_inflight = true;

    int mig_result = wait_for_migration_complete(qfd);
    if (mig_result < 0) {
        timing_log(LOG_ERROR, "마이그레이션 실패");
        goto cleanup;
    }
    migration_inflight = false;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 60;

    int join_ret = pthread_timedjoin_np(th, NULL, &ts);
    if (join_ret == ETIMEDOUT) {
        timing_log(LOG_ERROR, "덤프 스레드 타임아웃");
        ctx.cancel = 1;
        pthread_join(th, NULL);
    }
    thread_started = false;

    int final_error = ctx.error;
    char final_errmsg[256];
    pthread_mutex_lock(&ctx.mutex);
    snprintf(final_errmsg, sizeof(final_errmsg), "%s", ctx.errmsg);
    pthread_mutex_unlock(&ctx.mutex);

    if (ctx_initialized) {
        pthread_mutex_destroy(&ctx.mutex);
        ctx_initialized = false;
    }

    if (final_error) {
        timing_log(LOG_ERROR, "덤프 에러: %s", final_errmsg);
        goto cleanup;
    }

    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) {
        goto cleanup;
    }
    cont_sent = true;

    close(qfd);
    qfd = -1;

    if (compress_raw_snapshot(raw_path, outfile) < 0) {
        goto cleanup;
    }

    if (unlink(raw_path) < 0) {
        timing_log(LOG_WARN, "임시 덤프 삭제 실패: %s", strerror(errno));
    }

    timing_log(LOG_INFO, "savevm-gz 완료: %s", outfile);
    ret = 0;

cleanup:
    if (resp) {
        free(resp);
        resp = NULL;
    }
    if (migration_inflight && qfd >= 0) {
        qmp_simple_ok(qfd, "{\"execute\":\"migrate-cancel\"}");
    }
    if (thread_started) {
        ctx.cancel = 1;
        if (pipefd[1] >= 0) {
            close(pipefd[1]);
            pipefd[1] = -1;
        }
        pthread_join(th, NULL);
    }
    if (ctx_initialized) {
        pthread_mutex_destroy(&ctx.mutex);
    }
    if (!cont_sent && qfd >= 0) {
        qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
    }
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (raw_fd >= 0) close(raw_fd);
    if (raw_path) {
        if (ret != 0) {
            timing_log(LOG_INFO, "임시 덤프 보관: %s", raw_path);
        }
        free(raw_path);
    }
    if (qfd >= 0) {
        close(qfd);
    }
    return ret;
}

static int load_snapshot_internal(const char *snap_name) {
    if (!snap_name || !*snap_name) {
        timing_log(LOG_ERROR, "내부 스냅샷 이름이 비어있습니다");
        return -1;
    }

    timing_log(LOG_INFO, "내부 스냅샷 로드 시작: %s", snap_name);

    int ret = -1;
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) {
        return -1;
    }

    bool cont_sent = false;

    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) {
        goto cleanup;
    }

    if (hmp_load_snapshot(qfd, snap_name) < 0) {
        goto cleanup;
    }

    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) {
        goto cleanup;
    }
    cont_sent = true;

    timing_log(LOG_INFO, "내부 스냅샷 로드 완료: %s", snap_name);
    ret = 0;

cleanup:
    if (!cont_sent && qfd >= 0) {
        qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
    }
    if (qfd >= 0) {
        close(qfd);
    }
    return ret;
}

static int load_snapshot_gz(const char *infile) {
    timing_log(LOG_INFO, "loadvm-gz 시작: %s", infile);

    int ret = -1;
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) {
        return -1;
    }

    bool cont_sent = false;
    int pipefd[2] = { -1, -1 };
    pthread_t th;
    bool gzip_thread_started = false;
    GzipThreadCtx ctx;
    bool ctx_initialized = false;
    char *resp = NULL;

    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) {
        goto cleanup;
    }

    if (qmp_set_block_migration(qfd, g_block_migration)) {
        goto cleanup;
    }
    timing_log(LOG_INFO, "블록 마이그레이션: %s", g_block_migration ? "활성화" : "비활성화");

    if (pipe(pipefd) < 0) {
        timing_log(LOG_ERROR, "pipe 실패: %s", strerror(errno));
        goto cleanup;
    }

    #ifdef F_SETPIPE_SZ
    int pipe_size = PIPE_BUFFER_SIZE;
    if (fcntl(pipefd[1], F_SETPIPE_SZ, pipe_size) < 0) {
        timing_log(LOG_WARN, "파이프 버퍼 크기 설정 실패: %s", strerror(errno));
    } else {
        timing_log(LOG_DEBUG, "파이프 버퍼 크기: %d bytes", pipe_size);
    }
    #endif

    ctx = (GzipThreadCtx){
        .fd = pipefd[1],
        .path = strdup(infile),
        .error = 0,
        .errmsg = {0},
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cancel = 0,
        .bytes_processed = 0,
    };

    if (!ctx.path) {
        timing_log(LOG_ERROR, "경로 메모리 부족");
        goto cleanup;
    }
    ctx_initialized = true;

    if (qmp_getfd(qfd, pipefd[0], "snap") < 0) {
        goto cleanup;
    }
    close(pipefd[0]);
    pipefd[0] = -1;

    resp = qmp_cmd(qfd,
        "{\"execute\":\"migrate-incoming\",\"arguments\":{\"uri\":\"fd:snap\"}}"
    );
    if (!resp || looks_like_qmp_error(resp)) {
        if (resp && strstr(resp, "'-incoming' was not specified")) {
            timing_log(LOG_ERROR,
                       "migrate-incoming 실패: QEMU가 '-incoming' 옵션 없이 실행되었습니다 (%s)",
                       resp);
            timing_log(LOG_INFO,
                       "QEMU를 '-incoming defer' 옵션과 함께 실행한 뒤 다시 시도하세요.");
        } else {
            timing_log(LOG_ERROR, "migrate-incoming 명령 실패: %s", resp ? resp : "NULL");
        }
        goto cleanup;
    }
    free(resp);
    resp = NULL;

    if (pthread_create(&th, NULL, gzip_source_thread, &ctx) != 0) {
        timing_log(LOG_ERROR, "해제 스레드 생성 실패");
        qmp_simple_ok(qfd, "{\"execute\":\"migrate-cancel\"}");
        goto cleanup;
    }
    gzip_thread_started = true;

    int mig_result = wait_for_migration_complete(qfd);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 60;

    int join_ret = pthread_timedjoin_np(th, NULL, &ts);
    if (join_ret == ETIMEDOUT) {
        timing_log(LOG_ERROR, "gunzip 스레드 타임아웃");
        ctx.cancel = 1;
        pthread_join(th, NULL);
    }
    gzip_thread_started = false;

    int final_error = ctx.error;
    char final_errmsg[256];
    pthread_mutex_lock(&ctx.mutex);
    snprintf(final_errmsg, sizeof(final_errmsg), "%s", ctx.errmsg);
    pthread_mutex_unlock(&ctx.mutex);
    pthread_mutex_destroy(&ctx.mutex);
    ctx_initialized = false;

    if (mig_result < 0) {
        timing_log(LOG_ERROR, "복원 실패");
        goto cleanup;
    }

    if (final_error) {
        timing_log(LOG_ERROR, "gunzip 에러: %s", final_errmsg);
        goto cleanup;
    }

    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) {
        goto cleanup;
    }
    cont_sent = true;

    timing_log(LOG_INFO, "loadvm-gz 완료: %s", infile);
    ret = 0;

cleanup:
    if (resp) {
        free(resp);
        resp = NULL;
    }
    if (gzip_thread_started) {
        ctx.cancel = 1;
        pthread_join(th, NULL);
    }
    if (ctx_initialized) {
        pthread_mutex_destroy(&ctx.mutex);
    }
    if (ctx.path) {
        free(ctx.path);
        ctx.path = NULL;
    }
    if (pipefd[0] >= 0) {
        close(pipefd[0]);
    }
    if (pipefd[1] >= 0) {
        close(pipefd[1]);
    }
    if (qfd >= 0) {
        if (!cont_sent) {
            qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
        }
        close(qfd);
    }
    return ret;
}

static int delete_snapshot(const char *name) {
    if (!name) {
        timing_log(LOG_ERROR, "스냅샷 이름이 비었습니다");
        return -1;
    }

    int rc = 0;
    char sanitized[SNAP_NAME_MAX_LEN];
    sanitize_snapshot_name(name, sanitized, sizeof(sanitized));
    if (strcmp(name, sanitized) != 0) {
        timing_log(LOG_INFO, "내부 스냅샷 이름 정규화: '%s' -> '%s'", name, sanitized);
    }

    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd >= 0) {
        if (hmp_delete_snapshot(qfd, sanitized) < 0) {
            rc = -1;
        }
        close(qfd);
    } else {
        timing_log(LOG_WARN, "내부 스냅샷 삭제를 위해 QMP에 연결하지 못했습니다 (VM이 꺼져 있을 수 있음)");
    }

    char *path = snapshot_path_from_name(name);
    if (path) {
        if (unlink(path) == 0) {
            timing_log(LOG_INFO, "gzip 아카이브 삭제: %s", path);
        } else if (errno != ENOENT) {
            timing_log(LOG_ERROR, "%s 삭제 실패: %s", path, strerror(errno));
            rc = -1;
        }
        free(path);
    }

    return rc;
}

static int list_snapshot_archives(void) {
    const char *dir = g_snapshot_dir ? g_snapshot_dir : ".";
    DIR *d = opendir(dir);
    if (!d) {
        timing_log(LOG_ERROR, "%s: 디렉터리를 열 수 없습니다: %s", dir, strerror(errno));
        return -1;
    }

    printf("[gzip archives] %s\n", dir);

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

#ifdef DT_REG
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN && ent->d_type != DT_LNK) {
            continue;
        }
#endif

        size_t len = strlen(name);
        if (len >= 3 && strcmp(name + len - 3, ".gz") == 0) {
            printf("%s\n", name);
            found++;
        }
    }
    closedir(d);

    if (!found) {
        printf("(no gzip snapshots in %s)\n", dir);
    }
    return 0;
}

static int list_internal_snapshots(void) {
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) {
        timing_log(LOG_WARN, "내부 스냅샷 목록을 가져오지 못했습니다 (QMP 연결 실패)");
        return -1;
    }

    char *resp = hmp_command_raw(qfd, "info snapshots");
    close(qfd);
    if (!resp) {
        timing_log(LOG_WARN, "'info snapshots' 명령 실패");
        return -1;
    }

    if (hmp_response_is_error(resp)) {
        timing_log(LOG_WARN, "'info snapshots' 에러: %s", resp);
        free(resp);
        return -1;
    }

    printf("[internal snapshots]\n");
    hmp_print_return_stdout(resp);
    free(resp);
    return 0;
}

static int list_snapshots(void) {
    int rc = 0;
    if (list_internal_snapshots() != 0) {
        rc = -1;
    }
    if (list_snapshot_archives() != 0) {
        rc = -1;
    }
    return rc;
}


/* ---------- CLI ---------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --socket <path> <cmd> [name]\n"
        "       %s [--timelog <path>] --socket <path> <cmd> [name]\n"
        "  cmds:\n"
        "    savevm <name>      내부 스냅샷 + gzip 아카이브 생성\n"
        "    loadvm <name>      내부 스냅샷에서 즉시 복원\n"
        "    savevm-gz <gzip>   gzip 아카이브만 생성\n"
        "    loadvm-gz <gzip>   gzip 아카이브에서 복원 (-incoming 필요)\n"
        "    delvm  <name>\n"
        "    list\n"
        "  options:\n"
        "    --socket <path>    QMP 소켓 경로(기본: $HOME/vm/win11/qmp.sock)\n"
        "    --snapshot-dir <dir>  gzip 스냅샷 저장 디렉터리(기본: 현재 디렉터리)\n"
        "    --timelog <path>   스냅샷 작업 시간 로그 파일(기본: ./snapctl-timing.log)\n"
        "    --block-migration  마이그레이션 스트림에 블록 장치 포함 (기본: 비활성)\n",
        prog, prog);
}

int main(int argc, char **argv) {
    const char *socket_arg = NULL;
    const char *timelog_arg = NULL;
    int i = 1;
    
    while (i < argc) {
        if (!strcmp(argv[i], "--socket") && i+1 < argc) {
            socket_arg = argv[++i];
        } else if (!strcmp(argv[i], "--timelog") && i+1 < argc) {
            timelog_arg = argv[++i];
        } else if (!strcmp(argv[i], "--snapshot-dir") && i+1 < argc) {
            g_snapshot_dir = argv[++i];
        } else if (!strcmp(argv[i], "--block-migration")) {
            g_block_migration = true;
        } else break;
        i++;
    }

    if (!socket_arg) {
        const char *home = getenv("HOME");
        static char def[512];
        if (!home) die("HOME not set and --socket not provided");
        snprintf(def, sizeof(def), "%s/vm/win11/qmp.sock", home);
        g_sock_path = def;
    } else {
        g_sock_path = socket_arg;
    }

    if (!timelog_arg) {
        timelog_arg = "snapctl-timing.log";
    }

    if (!g_snapshot_dir) {
        g_snapshot_dir = ".";
    }
    if (ensure_dir_exists(g_snapshot_dir) < 0) {
        die("snapshot dir %s: %s", g_snapshot_dir, strerror(errno));
    }

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
        char *path = snapshot_path_from_name(snap_name);
        if (!path) {
            timing_log(LOG_ERROR, "스냅샷 이름이 잘못되었습니다: %s", snap_name);
            exit_code = 2;
            goto out;
        }

        char label[256];
        snprintf(label, sizeof(label), "snapctl savevm:%s", path);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        char sanitized[SNAP_NAME_MAX_LEN];
        sanitize_snapshot_name(snap_name, sanitized, sizeof(sanitized));
        if (strcmp(snap_name, sanitized) != 0) {
            timing_log(LOG_INFO, "내부 스냅샷 이름 정규화: '%s' -> '%s'", snap_name, sanitized);
        }

        int rc = save_snapshot_gz(path, sanitized, true);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "savevm '%s' %s (%.3f s)",
                   path, rc == 0 ? "성공" : "실패", elapsed_s);

        free(path);

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "loadvm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *snap_name = argv[i];
        char sanitized[SNAP_NAME_MAX_LEN];
        sanitize_snapshot_name(snap_name, sanitized, sizeof(sanitized));
        if (strcmp(snap_name, sanitized) != 0) {
            timing_log(LOG_INFO, "내부 스냅샷 이름 정규화: '%s' -> '%s'", snap_name, sanitized);
        }

        char *path = snapshot_path_from_name(snap_name);
        char label[256];
        snprintf(label, sizeof(label), "snapctl loadvm:%s", sanitized);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        int rc = load_snapshot_internal(sanitized);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "loadvm '%s' %s (%.3f s)",
                   sanitized, rc == 0 ? "성공" : "실패", elapsed_s);

        if (rc != 0 && path && access(path, R_OK) == 0) {
            timing_log(LOG_INFO,
                       "gz 아카이브가 존재합니다: %s (필요시 'loadvm-gz' 사용)",
                       path);
        }

        free(path);

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "savevm-gz")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *gz_path = argv[i];
        char label[256];
        snprintf(label, sizeof(label), "snapctl savevm-gz:%s", gz_path);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        int rc = save_snapshot_gz(gz_path, NULL, false);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "savevm-gz '%s' %s (%.3f s)",
                   gz_path, rc == 0 ? "성공" : "실패", elapsed_s);

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "loadvm-gz")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        const char *gz_path = argv[i];
        char label[256];
        snprintf(label, sizeof(label), "snapctl loadvm-gz:%s", gz_path);

        TimingContext *ctx = timing_start(label);
        if (!ctx) {
            timing_log(LOG_WARN, "타이밍 컨텍스트를 만들 수 없어 측정을 건너뜁니다.");
        }

        int rc = load_snapshot_gz(gz_path);

        double elapsed_s = 0.0;
        if (ctx) {
            elapsed_s = timing_end(ctx);
        }

        timing_log(rc == 0 ? LOG_INFO : LOG_ERROR,
                   "loadvm-gz '%s' %s (%.3f s)",
                   gz_path, rc == 0 ? "성공" : "실패", elapsed_s);

        exit_code = (rc == 0) ? 0 : 2;
        goto out;
    } else if (!strcmp(cmd, "delvm")) {
        if (i >= argc) { usage(argv[0]); exit_code = 1; goto out; }
        exit_code = delete_snapshot(argv[i]) == 0 ? 0 : 2;
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
