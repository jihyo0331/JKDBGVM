#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include "hmp.h"
#include "qmp.h"  
#include "timelog.h"


/*
 * HMP(Command) → QMP(JSON) 변환 후 전송
 *   예: {"execute":"human-monitor-command","arguments":{"command-line":"savevm snap1"}}
 */
char *hmp_command_raw(int qmp_fd, const char *cmdline) {
    if (!cmdline) return NULL;

    char *payload = NULL;
    if (asprintf(&payload,
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}",
                 cmdline) < 0)
        return NULL;

    char *resp = qmp_cmd(qmp_fd, "%s", payload);
    free(payload);
    return resp;
}

/* QMP JSON 응답에서 오류 여부 판별 */
bool hmp_response_is_error(const char *resp) {
    if (!resp) return true;
    if (looks_like_qmp_error(resp)) return true;
    if (strstr(resp, "Error:") || strstr(resp, "error:")) return true;
    return false;
}

/* QMP 응답에서 "return" 필드의 문자열을 stdout으로 출력 */
void hmp_print_return_stdout(const char *resp) {
    if (!resp) return;

    const char *p = strstr(resp, "\"return\"");
    if (!p) return;

    p = strchr(p, ':');
    if (!p) return;
    ++p;

    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '"') return;
    ++p;

    bool printed = false;
    char last = 0;

    while (*p && *p != '"') {
        if (*p == '\\') {
            ++p;
            if (!*p) break;
            char c = *p++;
            switch (c) {
            case 'n': putchar('\n'); printed = true; last = '\n'; break;
            case 'r': break; /* ignore CR */
            case 't': putchar('\t'); printed = true; last = '\t'; break;
            case '\\': putchar('\\'); printed = true; last = '\\'; break;
            case '"': putchar('"'); printed = true; last = '"'; break;
            default: putchar(c); printed = true; last = c; break;
            }
        } else {
            putchar(*p++);
            printed = true;
            last = *(p - 1);
        }
    }

    if (printed && last != '\n') putchar('\n');
}

/* 명령 실행 후 에러 체크 */
int hmp_command_check(int qmp_fd, const char *cmdline) {
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

/* 내부 스냅샷 생성 */
int hmp_save_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) return -1;

    char *cmdline = NULL;
    if (asprintf(&cmdline, "savevm %s", snap_name) < 0) return -1;
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);

    if (rc == 0)
        timing_log(LOG_INFO, "내부 스냅샷 생성 완료: %s", snap_name);
    return rc;
}

/* 내부 스냅샷 복원 */
int hmp_load_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) return -1;

    char *cmdline = NULL;
    if (asprintf(&cmdline, "loadvm %s", snap_name) < 0) return -1;
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);

    if (rc == 0)
        timing_log(LOG_INFO, "내부 스냅샷 복원 완료: %s", snap_name);
    return rc;
}

/* 내부 스냅샷 삭제 */
int hmp_delete_snapshot(int qmp_fd, const char *snap_name) {
    if (!snap_name || !*snap_name) return -1;

    char *cmdline = NULL;
    if (asprintf(&cmdline, "delvm %s", snap_name) < 0) return -1;
    int rc = hmp_command_check(qmp_fd, cmdline);
    free(cmdline);

    if (rc == 0)
        timing_log(LOG_INFO, "내부 스냅샷 삭제 완료: %s", snap_name);
    return rc;
}
