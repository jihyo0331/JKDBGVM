#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>

#include "snapctl.h"  
#include "snapshot.h"  
#include "timelog.h"


/* usage / die는 main의 고유 책임(입출력)*/
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

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
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
