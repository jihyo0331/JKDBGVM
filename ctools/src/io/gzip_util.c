// gzip_util.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <zlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "gzip_util.h"
#include "snapctl.h"   
#include "timelog.h"



/* ---------- pigz helper ---------- */
bool program_exists(const char *prog) {
    if (!prog || !*prog) return false;
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return false;

    char *paths = strdup(path_env);
    if (!paths) return false;

    bool found = false;
    char *saveptr = NULL;
    for (char *token = strtok_r(paths, ":", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ":", &saveptr)) {
        if (!*token) continue;
        char candidate[PATH_MAX];
        snprintf(candidate, sizeof(candidate), "%s/%s", token, prog);
        if (access(candidate, X_OK) == 0) { found = true; break; }
    }

    free(paths);
    return found;
}

bool pigz_available(void) {
    static int cached = -1;
    if (cached == -1) {
        cached = program_exists("pigz") ? 1 : 0;
        if (!cached)
            timing_log(LOG_WARN, "pigz not found in PATH, falling back to zlib");
    }
    return cached != 0;
}

int pigz_thread_count(void) {
    long cpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu < 1) cpu = 1;
    if (cpu > 64) cpu = 64;
    return (int)cpu;
}


void gzip_thread_set_error(GzipThreadCtx *ctx, const char *fmt, ...) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->error = -1;
    va_list ap; va_start(ap, fmt);
    vsnprintf(ctx->errmsg, sizeof(ctx->errmsg), fmt, ap);
    va_end(ap);
    pthread_mutex_unlock(&ctx->mutex);
}

/* ---------- gzip 해제 스레드 ---------- */
void *gzip_source_thread(void *opaque) {
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
                if (dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(EXIT_FAILURE);
                close(pipefd[1]);
                execlp("pigz", "pigz", "-d", "-c", "-p", threads_str, ctx->path, (char *)NULL);
                _exit(127);
            } else if (pigz_pid < 0) {
                timing_log(LOG_WARN, "pigz fork 실패: %s", strerror(errno));
                close(pipefd[0]); close(pipefd[1]);
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
                if (errno == EINTR) continue;
                gzip_thread_set_error(ctx, "pigz 파이프 읽기 실패: %s", strerror(errno));
                break;
            }
        } else {
            int gn = gzread(gz, buf, sizeof(buf));
            if (gn < 0) {
                int zr; const char *msg = gzerror(gz, &zr);
                gzip_thread_set_error(ctx, "gzread 실패: %s", msg);
                break;
            }
            n = gn;
        }

        if (n == 0) {
            timing_log(LOG_INFO, "압축 해제 완료: %.2f MB", ctx->bytes_processed / (1024.0 * 1024.0));
            break;
        }

        ssize_t off = 0;
        while (off < n && !ctx->cancel) {
            ssize_t w = write(ctx->fd, buf + off, (size_t)(n - off));
            if (w > 0) off += w;
            else if (w < 0 && errno == EINTR) continue;
            else {
                gzip_thread_set_error(ctx, "pipe 쓰기 실패: %s", strerror(errno));
                goto cleanup;
            }
        }
        ctx->bytes_processed += (size_t)n;
    }

cleanup:
    if (use_pigz) {
        if (pigz_fd >= 0) close(pigz_fd);
        if (pigz_pid > 0) {
            int status;
            pid_t waited = waitpid(pigz_pid, &status, WNOHANG);
            if (waited == 0) waitpid(pigz_pid, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                gzip_thread_set_error(ctx, "pigz 종료 코드 %d",
                                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        }
    } else {
        if (gz) gzclose(gz);
    }

    close(ctx->fd);
    return NULL;
}

/* ---------- gzip 압축 ---------- */
int compress_raw_snapshot(const char *raw_path, const char *gz_path) {
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
            if (out_fd < 0) _exit(EXIT_FAILURE);
            if (dup2(out_fd, STDOUT_FILENO) < 0) _exit(EXIT_FAILURE);
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
                if (rename(tmp_path, gz_path) < 0)
                    timing_log(LOG_ERROR, "임시 파일 이동 실패: %s", strerror(errno));
                else {
                    timing_log(LOG_INFO, "pigz 압축 완료 (%d threads): %s", threads, gz_path);
                    ret = 0; goto out;
                }
            } else {
                timing_log(LOG_WARN, "pigz 종료 코드 %d",
                           WIFEXITED(status) ? WEXITSTATUS(status) : -1);
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
                int zr; const char *msg = gzerror(gz, &zr);
                timing_log(LOG_ERROR, "gzwrite 실패 (%zd bytes): %s", n, msg);
                goto out;
            }
            total += (size_t)n;
        } else if (n == 0) break;
        else if (errno == EINTR) continue;
        else {
            timing_log(LOG_ERROR, "원본 읽기 실패: %s", strerror(errno));
            goto out;
        }
    }

    if (gzclose(gz) != Z_OK) {
        gz = NULL;
        timing_log(LOG_ERROR, "gzclose 실패");
        goto out;
    }

    close(raw_fd);
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
    if (tmp_path) { if (ret != 0) unlink(tmp_path); free(tmp_path); }
    return ret;
}
