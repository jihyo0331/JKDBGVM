// src/dump.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>

#include "snapctl.h"
#include "timelog.h"   // 로그 출력에 사용
#include "dump.h"

#ifndef READ_BUFSZ
#define READ_BUFSZ 65536
#endif

// 진행 로그 주기(바이트) — 너무 자주 찍지 않도록 256MB마다
#ifndef DUMP_PROGRESS_BYTES
#define DUMP_PROGRESS_BYTES (256ULL * 1024ULL * 1024ULL)
#endif

struct DumpWriter {
    int pipe_fd, out_fd;
    char path[512];
    volatile int error, cancel;
    char errmsg[256];
    size_t bytes_processed;
    pthread_mutex_t mutex;
};

static void dump_set_error(struct DumpWriter *dw, const char *fmt, ...)
    __attribute__((format(printf,2,3)));

static void dump_set_error(struct DumpWriter *dw, const char *fmt, ...) {
    pthread_mutex_lock(&dw->mutex);
    dw->error = -1;
    va_list ap; va_start(ap, fmt);
    vsnprintf(dw->errmsg, sizeof(dw->errmsg), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&dw->mutex);
}

static void* dump_thread(void *arg) {
    struct DumpWriter *dw = arg;
    unsigned char buf[READ_BUFSZ];

    // SIGPIPE 무시(파이프 소비자 종료 시 write 에러 처리)
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    dw->bytes_processed = 0;
    size_t next_progress_mark = DUMP_PROGRESS_BYTES;

    timing_log(LOG_INFO, "dump start → %s", dw->path[0] ? dw->path : "(fd)");

    while (!dw->cancel) {
        ssize_t n = read(dw->pipe_fd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t off = 0;
            while (off < n && !dw->cancel) {
                ssize_t w = write(dw->out_fd, buf + off, (size_t)(n - off));
                if (w > 0) off += w;
                else if (w < 0 && errno == EINTR) continue;
                else { dump_set_error(dw, "write failed: %s", strerror(errno)); goto out; }
            }
            dw->bytes_processed += (size_t)n;

            // 주기적 진행 로그 (DEBUG)
            if (dw->bytes_processed >= next_progress_mark) {
                timing_log(LOG_DEBUG, "dump progress: %.2f MB",
                           dw->bytes_processed / (1024.0 * 1024.0));
                // 다음 마크 갱신(오버플로우 방지)
                if (SIZE_MAX - next_progress_mark > DUMP_PROGRESS_BYTES)
                    next_progress_mark += DUMP_PROGRESS_BYTES;
                else
                    next_progress_mark = SIZE_MAX;
            }
        } else if (n == 0) {
            // EOF
            break;
        } else if (errno != EINTR) {
            dump_set_error(dw, "read failed: %s", strerror(errno));
            break;
        }
    }

out:
    if (!dw->error && fsync(dw->out_fd) < 0)
        dump_set_error(dw, "fsync failed: %s", strerror(errno));

    close(dw->out_fd);
    close(dw->pipe_fd);

    if (dw->error) {
        timing_log(LOG_ERROR, "dump error: %s", dw->errmsg[0] ? dw->errmsg : "unknown");
    } else {
        timing_log(LOG_INFO, "dump done: %.2f MB",
                   dw->bytes_processed / (1024.0 * 1024.0));
    }
    return NULL;
}

int dump_writer_start(int pipe_fd, int out_fd, const char *path,
                      DumpWriter **out_dw, pthread_t *out_thread)
{
    struct DumpWriter *dw = calloc(1, sizeof(*dw));
    if (!dw) return -1;

    dw->pipe_fd = pipe_fd;
    dw->out_fd  = out_fd;
    if (path) strncpy(dw->path, path, sizeof(dw->path)-1);
    pthread_mutex_init(&dw->mutex, NULL);

    pthread_t th;
    if (pthread_create(&th, NULL, dump_thread, dw) != 0) {
        timing_log(LOG_ERROR, "dump thread create failed");
        pthread_mutex_destroy(&dw->mutex);
        free(dw);
        return -1;
    }

    *out_dw = dw;
    *out_thread = th;
    return 0;
}

int dump_writer_join(DumpWriter *dw, pthread_t th, int timeout_sec,
                     int *out_error, char out_errmsg[256], size_t *out_bytes)
{
#ifdef __linux__
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_sec;
    int j = pthread_timedjoin_np(th, NULL, &ts);
    if (j == ETIMEDOUT) {
        timing_log(LOG_ERROR, "dump thread timed out (%ds), cancelling…", timeout_sec);
        dw->cancel = 1;
        pthread_join(th, NULL);
    }
#else
    pthread_join(th, NULL);
#endif

    pthread_mutex_lock(&dw->mutex);
    if (out_error)  *out_error  = dw->error;
    if (out_errmsg) {
        if (dw->errmsg[0]) snprintf(out_errmsg, 256, "%s", dw->errmsg);
        else out_errmsg[0] = '\0';
    }
    if (out_bytes)  *out_bytes  = dw->bytes_processed;
    pthread_mutex_unlock(&dw->mutex);

    return 0;
}

void dump_writer_destroy(struct DumpWriter *dw) {
    if (!dw) return;
    pthread_mutex_destroy(&dw->mutex);
    free(dw);
}


/*

int dump_writer_join(DumpWriter *dw, pthread_t th, int timeout_sec,
                     int *out_error, char out_errmsg[256], size_t *out_bytes) 
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += timeout_sec;
    int j = pthread_timedjoin_np(th, NULL, &ts);
    if (j == ETIMEDOUT) { dw->cancel = 1; pthread_join(th, NULL); }

    pthread_mutex_lock(&dw->mutex);
    if (out_error) *out_error = dw->error;
    if (out_errmsg) snprintf(out_errmsg, 256, "%s", dw->errmsg);
    if (out_bytes) *out_bytes = dw->bytes_processed;
    pthread_mutex_unlock(&dw->mutex);
    return 0;
}

*/