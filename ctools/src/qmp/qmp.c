// src/qmp.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdarg.h>
#include <sys/time.h> 
#include <sys/uio.h>

#include "qmp.h"
#include "snapctl.h"  
#include "timelog.h"


/* --------- 콘솔 전용 로깅(파일 없음) --------- */
void log_msg(const char *level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", level);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ---------- 연결 ---------- */
int set_timeouts(int fd, int r_ms, int w_ms) {
    struct timeval r = { .tv_sec = r_ms / 1000, .tv_usec = (r_ms % 1000) * 1000 };
    struct timeval w = { .tv_sec = w_ms / 1000, .tv_usec = (w_ms % 1000) * 1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w)) < 0) return -1;
    return 0;
}

int qmp_connect(const char *sockpath) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    set_timeouts(fd, READ_TIMEOUT_MS, WRITE_TIMEOUT_MS);
    return fd;
}

int qmp_open_and_negotiate(ReadBuffer *rb) {
    for (int i = 0; i < QMP_HANDSHAKE_RETRY; i++) {
        int fd = qmp_connect(g_sock_path);
        if (fd < 0) { sleep_ms(RETRY_BACKOFF_MS); continue; }

        char *banner = read_line_buffered(fd, rb);
        if (!banner) { close(fd); continue; }
        free(banner);

        if (send_line(fd, "{\"execute\":\"qmp_capabilities\"}") < 0) { close(fd); continue; }

        char *neg = read_resp_line(fd, rb);
        if (!neg) { close(fd); continue; }
        free(neg);
        return fd;
    }
    log_msg("ERROR", "QMP handshake 실패: 소켓 %s", g_sock_path);
    return -1;
}

/* ---------- 명령 ---------- */
char *qmp_cmd(int fd, const char *fmt, ...) {
    va_list ap;
    char *payload = NULL;
    va_start(ap, fmt);
    vasprintf(&payload, fmt, ap);
    va_end(ap);

    if (!payload) return NULL;
    if (send_line(fd, payload) < 0) { free(payload); return NULL; }
    free(payload);

    ReadBuffer rb;
    init_read_buffer(&rb);
    return read_resp_line(fd, &rb);
}

int qmp_simple_ok(int qmp_fd, const char *json) {
    char *resp = qmp_cmd(qmp_fd, "%s", json);
    if (!resp) return -1;
    bool err = strstr(resp, "\"error\"") != NULL;
    if (err) log_msg("ERROR", "QMP 명령 에러(%s): %s", json, resp);
    free(resp);
    return err ? -1 : 0;
}

/* ---------- Capability ---------- */
int qmp_set_block_migration(int qmp_fd, bool enable) {
    const char *json = enable
        ? "{\"execute\":\"migrate-set-capabilities\",\"arguments\":{\"capabilities\":[{\"capability\":\"block\",\"state\":true}]}}"
        : "{\"execute\":\"migrate-set-capabilities\",\"arguments\":{\"capabilities\":[{\"capability\":\"block\",\"state\":false}]}}";
    char *resp = qmp_cmd(qmp_fd, "%s", json);
    if (!resp) return -1;

    if (strstr(resp, "\"error\"")) {
        if (strstr(resp, "CapabilityNotAvailable")) {
            log_msg("INFO", "블록 마이그레이션 capability 미지원: %s", resp);
            free(resp);
            return 0; // 미지원은 오류로 보지 않음
        }
        log_msg("ERROR", "QMP 명령 에러(%s): %s", json, resp);
        free(resp);
        return -1;
    }
    free(resp);
    return 0;
}

/* ---------- 상태 검사 ---------- */
const char *qmp_skip_ws(const char *p) { while (p && *p && isspace((unsigned char)*p)) ++p; return p; }

bool qmp_json_bool(const char *json, const char *key, bool expected) {
    if (!json || !key) return false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pattern))) {
        const char *c = qmp_skip_ws(p + strlen(pattern));
        if (*c++ != ':') { p++; continue; }
        c = qmp_skip_ws(c);
        if ((expected && strncmp(c, "true", 4) == 0) || (!expected && strncmp(c, "false", 5) == 0))
            return true;
        p++;
    }
    return false;
}

bool qmp_json_string(const char *json, const char *key, const char *value) {
    if (!json || !key || !value) return false;
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = json;
    size_t vlen = strlen(value);
    while ((p = strstr(p, pattern))) {
        const char *c = qmp_skip_ws(p + strlen(pattern));
        if (*c++ != ':') { p++; continue; }
        c = qmp_skip_ws(c);
        if (*c++ != '"') { p++; continue; }
        if (strncmp(c, value, vlen) == 0 && c[vlen] == '"') return true;
        p++;
    }
    return false;
}

bool qmp_status_is_running(const char *status_json) {
    return qmp_json_bool(status_json, "running", true)
        || qmp_json_string(status_json, "status", "running");
}

bool qmp_status_is_postmigrate(const char *status_json) {
    return qmp_json_string(status_json, "status", "postmigrate")
        || qmp_json_bool(status_json, "postmigrate", true)
        || qmp_json_string(status_json, "status", "paused");
}

// paused 판단: status=paused 이거나 running=false 로 표기하는 QEMU 대응
bool qmp_status_is_paused(const char *j) {
    return qmp_json_string(j, "status", "paused")
        || qmp_json_bool(j, "running", false);
}

// 마이그 관련 중간 상태들: 버전별 표기 차이 흡수
bool qmp_status_is_inmigrate(const char *j) {
    return qmp_json_string(j, "status", "inmigrate")
        || qmp_json_string(j, "status", "prelaunch")
        || qmp_json_string(j, "status", "postmigrate")
        || qmp_json_string(j, "status", "suspended");
}

// 실제 migrate 진행 중인지 확인(취소 판단용)
bool qmp_migration_active(int qfd) {
    char *r = qmp_cmd(qfd, "{\"execute\":\"query-migrate\"}");
    if (!r) return false;
    bool active = strstr(r, "\"status\":\"active\"")
               || strstr(r, "\"status\":\"setup\"");
    free(r);
    return active;
}


/* ---------- 복구 ---------- */
void qmp_try_log_cancel(int qmp_fd, const char *context) {
    (void)context; // 문맥 문자열은 지금은 사용 안 함
    char *resp = qmp_cmd(qmp_fd, "{\"execute\":\"migrate-cancel\"}");
    if (resp) free(resp);
}

int qmp_ensure_running(int qmp_fd, const char *context) {
    bool tried_cont = false;

    for (int i = 0; i < 8; i++) {
        char *st = qmp_cmd(qmp_fd, "{\"execute\":\"query-status\"}");
        if (!st) { sleep_ms(120); continue; }

        // 상태 로그(디버깅에 도움)
        const char *p = strstr(st, "\"status\"");
        if (p) log_msg("INFO", "%s: query-status: %s", context, st);

        // 이미 실행 중
        if (qmp_status_is_running(st)) { free(st); return 0; }

        // 마이그 중(active/setup)일 때만 cancel
        if (qmp_status_is_inmigrate(st) && qmp_migration_active(qmp_fd)) {
            log_msg("INFO", "%s: migration active → migrate-cancel", context);
            free(st);
            qmp_try_log_cancel(qmp_fd, context);
            sleep_ms(200);
            continue;
        }

        // paused/postmigrate면 cont 한 번만 강하게 시도하고, 짧게 재확인 루프
        if (!tried_cont && (qmp_status_is_paused(st) || qmp_status_is_postmigrate(st))) {
            log_msg("INFO", "%s: paused/postmigrate → cont", context);
            tried_cont = true;
            free(st);

            // cont 보냄(응답은 베스트에포트로 무시)
            char *resp = qmp_cmd(qmp_fd, "{\"execute\":\"cont\"}");
            if (resp) free(resp);

            // cont 후 5회 이내 재확인
            for (int j = 0; j < 5; j++) {
                sleep_ms(200);
                char *st2 = qmp_cmd(qmp_fd, "{\"execute\":\"query-status\"}");
                if (!st2) continue;
                if (qmp_status_is_running(st2)) { free(st2); return 0; }
                // 여전히 paused 계열이면 한 번 더 대기
                free(st2);
            }
            // 여기까지 오면 다음 바깥 루프로 재시도
            continue;
        }

        log_msg("WARN", "%s: 실행 아님 → 재시도 (status 스냅샷 위 로그 참조)", context);
        free(st);
        sleep_ms(200);
    }

    log_msg("ERROR", "%s: VM 실행 복구 실패", context);
    return -1;
}



bool looks_like_qmp_error(const char *resp) {
    if (!resp) return true;
    if (strstr(resp, "\"error\"")) return true;   // 표준 QMP 에러 필드
    if (strstr(resp, "Error:") || strstr(resp, "error:")) return true; // HMP 메시지 섞임 방지
    return false;
}


int qmp_getfd(int qmp_fd, int fd_to_send, const char *fdname) {
    if (!fdname || !*fdname) fdname = "snap";

    char *json = NULL;
    if (asprintf(&json,
        "{\"execute\":\"getfd\",\"arguments\":{\"fdname\":\"%s\"}}\n", fdname) < 0) {
        return -1;
    }

    struct iovec iov = { .iov_base = json, .iov_len = strlen(json) };

    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));

    struct msghdr msg = {0};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    int rc = sendmsg(qmp_fd, &msg, 0);
    free(json);
    return (rc < 0) ? -1 : 0;
}

