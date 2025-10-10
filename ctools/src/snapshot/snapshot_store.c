#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <ctype.h>


#include "snapctl.h"
#include "timelog.h"
#include "dump.h"
#include "snapshot_list.h"
#include "qmp.h"
#include "hmp.h"
#include "gzip_util.h"

/* ---------------- 파일 경로 유틸 ---------------- */
int ensure_dir_exists(const char *dir) {
    if (!dir || !*dir) return 0;
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }
    if (errno != ENOENT) return -1;
    return (mkdir(dir, 0755) == 0) ? 0 : -1;
}

char* snapshot_path_from_name(const char *name) {
    const char *dir = g_snapshot_dir ? g_snapshot_dir : ".";
    if (!name || !*name) return NULL;

    size_t len = strlen(name);
    bool has_ext = (len >= 3 && strcmp(name + len - 3, ".gz") == 0);

    if (strchr(name, '/')) return strdup(name);  // 절대/상대 경로 그대로 사용

    char *path = NULL;
    if (has_ext)
        asprintf(&path, "%s/%s", dir, name);
    else
        asprintf(&path, "%s/%s.gz", dir, name);
    return path;
}

void sanitize_snapshot_name(const char *name, char *out, size_t out_sz) {
    size_t pos = 0;
    if (!out || out_sz == 0) return;
    if (!name) name = "";
    for (size_t i = 0; name[i] && pos < out_sz - 1; ++i) {
        unsigned char c = (unsigned char)name[i];
        out[pos++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
    }
    if (pos == 0) {
        const char *fallback = "snap";
        while (*fallback && pos < out_sz - 1)
            out[pos++] = *fallback++;
    }
    out[pos] = '\0';
}


/* ---------------- 스냅샷 저장 ---------------- */
int save_snapshot_gz(const char *outfile, const char *hmp_name, bool create_internal) {
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) return -1;

    int ret = -1;
    bool cont_sent = false;
    bool migration_inflight = false;
    int pipefd[2] = { -1, -1 };
    char *raw_path = NULL;
    int raw_fd = -1;
    DumpWriter *dw = NULL;
    pthread_t th;
    int derr = 0; char dmsg[256]; size_t dbytes = 0;
    char *resp = NULL;

    /* --- QMP 준비 --- */
    if (qmp_ensure_running(qfd, "savevm-gz") < 0) goto cleanup;
    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) goto cleanup;

    /* --- 내부 스냅샷 생성 (옵션) --- */
    if (create_internal && hmp_name && *hmp_name) {
        if (hmp_save_snapshot(qfd, hmp_name) < 0) goto cleanup;
    }

    /* --- 파이프 생성 --- */
    if (pipe(pipefd) < 0) goto cleanup;

#ifdef F_SETPIPE_SZ
    fcntl(pipefd[0], F_SETPIPE_SZ, PIPE_BUFFER_SIZE);
#endif

    /* --- 임시 RAW 덤프 파일 경로 --- */
    if (asprintf(&raw_path, "%s.rawtmp", outfile) < 0) goto cleanup;
    raw_fd = open(raw_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (raw_fd < 0) goto cleanup;

    /* --- 덤프 스레드 시작 --- */
    if (dump_writer_start(pipefd[0], raw_fd, raw_path, &dw, &th) != 0) {
        timing_log(LOG_ERROR, "덤프 스레드 생성 실패");
        goto cleanup;
    }
    pipefd[0] = -1;
    raw_fd = -1;

    /* --- QMP에 FD 전달 --- */
    if (qmp_getfd(qfd, pipefd[1], "snap") < 0) goto cleanup;
    close(pipefd[1]);
    pipefd[1] = -1;

    /* --- 마이그레이션 명령 실행 --- */
    resp = qmp_cmd(qfd, "{\"execute\":\"migrate\",\"arguments\":{\"uri\":\"fd:snap\"}}");
    if (!resp || looks_like_qmp_error(resp)) goto cleanup;
    free(resp); resp = NULL;
    migration_inflight = true;

    if (wait_for_migration_complete(qfd) < 0) goto cleanup;
    migration_inflight = false;

    /* --- 덤프 완료 대기 --- */
    dump_writer_join(dw, th, 60, &derr, dmsg, &dbytes);
    dump_writer_destroy(dw);
    dw = NULL;

    if (derr) {
        timing_log(LOG_ERROR, "dump error: %s", dmsg);
        goto cleanup;
    }

    /* --- VM 재개 --- */
    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) goto cleanup;
    cont_sent = true;

    close(qfd);
    qfd = -1;

    /* --- gzip 압축 --- */
    if (compress_raw_snapshot(raw_path, outfile) < 0) goto cleanup;
    unlink(raw_path);
    ret = 0;

cleanup:
    if (resp) free(resp);
    if (migration_inflight && qfd >= 0)
        qmp_simple_ok(qfd, "{\"execute\":\"migrate-cancel\"}");
    if (!cont_sent && qfd >= 0)
        qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (raw_fd >= 0) close(raw_fd);
    if (raw_path) free(raw_path);
    if (dw) dump_writer_destroy(dw);
    if (qfd >= 0) close(qfd);
    return ret;
}
