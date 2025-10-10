#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "snapshot.h"
#include "qmp.h"
#include "hmp.h"
#include "gzip_util.h"
#include "timelog.h"
#include "snapctl.h"


// 내부 스냅샷 로드 (HMP: loadvm)
int load_snapshot_internal(const char *snap_name) {
    if (!snap_name || !*snap_name) {
        timing_log(LOG_ERROR, "내부 스냅샷 이름이 비어있습니다");
        return -1;
    }

    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) return -1;

    int ret = -1;
    bool cont_sent = false;

    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) goto cleanup;
    if (hmp_load_snapshot(qfd, snap_name) < 0) goto cleanup;
    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) goto cleanup;
    qmp_ensure_running(qfd, "loadvm-internal"); 
    cont_sent = true;

    timing_log(LOG_INFO, "내부 스냅샷 로드 완료: %s", snap_name);
    ret = 0;

cleanup:
    if (!cont_sent && qfd >= 0) {
        qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
    }
    if (qfd >= 0) close(qfd);
    return ret;
}

// gzip 아카이브에서 로드 (migrate-incoming + gunzip/pigz 스트리밍)
int load_snapshot_gz(const char *infile) {
    if (!infile || !*infile) {
        timing_log(LOG_ERROR, "gzip 스냅샷 경로가 비어있습니다");
        return -1;
    }

    int ret = -1;
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) return -1;

    bool cont_sent = false;
    int pipefd[2] = { -1, -1 };
    pthread_t th;
    bool gzip_thread_started = false;
    GzipThreadCtx ctx;
    bool ctx_initialized = false;
    char *resp = NULL;

    // VM 정지 및 블록 마이그레이션 capability
    if (qmp_simple_ok(qfd, "{\"execute\":\"stop\"}")) goto cleanup;
    if (qmp_set_block_migration(qfd, g_block_migration)) goto cleanup;
    timing_log(LOG_INFO, "블록 마이그레이션: %s", g_block_migration ? "활성화" : "비활성화");

    // 스트림 파이프
    if (pipe(pipefd) < 0) goto cleanup;
#ifdef F_SETPIPE_SZ
    fcntl(pipefd[1], F_SETPIPE_SZ, PIPE_BUFFER_SIZE);
#endif

    // gunzip/pigz 스레드 컨텍스트
    ctx = (GzipThreadCtx){
        .fd = pipefd[1],
        .path = strdup(infile),
        .error = 0,
        .errmsg = {0},
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .cancel = 0,
        .bytes_processed = 0,
    };
    if (!ctx.path) goto cleanup;
    ctx_initialized = true;

    // QEMU 쪽으로 read end 전달
    if (qmp_getfd(qfd, pipefd[0], "snap") < 0) goto cleanup;
    close(pipefd[0]); pipefd[0] = -1;

        // gunzip/pigz 스레드 시작(압축해제 → pipefd[1]로 write)
    if (pthread_create(&th, NULL, gzip_source_thread, &ctx) != 0) {
        qmp_simple_ok(qfd, "{\"execute\":\"migrate-cancel\"}");
        goto cleanup;
    }
    gzip_thread_started = true;

    // 수신 준비
    resp = qmp_cmd(qfd, "{\"execute\":\"migrate-incoming\",\"arguments\":{\"uri\":\"fd:snap\"}}");
    if (!resp || looks_like_qmp_error(resp)) {
        if (resp && strstr(resp, "'-incoming' was not specified")) {
            timing_log(LOG_ERROR, "migrate-incoming 실패: QEMU가 '-incoming' 없이 실행됨 (%s)", resp);
            timing_log(LOG_INFO, "QEMU를 '-incoming defer' 옵션과 함께 실행하세요.");
        } else {
            timing_log(LOG_ERROR, "migrate-incoming 실패: %s", resp ? resp : "NULL");
        }
        goto cleanup;
    }
    free(resp); resp = NULL;

    // 마이그레이션 완료 대기
    {
        int mig_result = wait_for_migration_complete(qfd);
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 60;
        if (pthread_timedjoin_np(th, NULL, &ts) == ETIMEDOUT) {
            ctx.cancel = 1;
            pthread_join(th, NULL);
        }
        gzip_thread_started = false;

        // 스레드 에러/마이그 실패 체크
        if (mig_result < 0) goto cleanup;
        if (ctx.error) {
            timing_log(LOG_ERROR, "gunzip 에러: %s", ctx.errmsg);
            goto cleanup;
        }
    }

    // 재개
    if (qmp_simple_ok(qfd, "{\"execute\":\"cont\"}")) goto cleanup;
    qmp_ensure_running(qfd, "loadvm-gz"); 
    cont_sent = true;

    timing_log(LOG_INFO, "loadvm-gz 완료: %s", infile);
    ret = 0;

cleanup:
    if (resp) free(resp);
    if (gzip_thread_started) { ctx.cancel = 1; pthread_join(th, NULL); }
    if (ctx_initialized) pthread_mutex_destroy(&ctx.mutex);
    if (ctx.path) { free(ctx.path); ctx.path = NULL; }
    if (pipefd[0] >= 0) close(pipefd[0]);
    if (pipefd[1] >= 0) close(pipefd[1]);
    if (qfd >= 0) {
        if (!cont_sent) qmp_simple_ok(qfd, "{\"execute\":\"cont\"}");
        qmp_ensure_running(qfd, "loadvm-gz:cleanup"); 
        close(qfd);
    }
    return ret;
}
