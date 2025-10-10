#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "snapctl.h"

typedef struct {
    char buffer[READ_BUFSZ];
    char *buf_ptr;
    size_t buf_len;
} ReadBuffer;

/* 연결 + 네고 */
int  qmp_open_and_negotiate(ReadBuffer *rb);

/* 명령 */
char *qmp_cmd(int fd, const char *fmt, ...);
int   qmp_simple_ok(int qmp_fd, const char *json);
int   qmp_set_block_migration(int qmp_fd, bool enable);
int   qmp_getfd(int qmp_fd, int fd_to_send, const char *fdname);

/* 상태/파서 */
bool  looks_like_qmp_error(const char *resp);
bool  qmp_json_bool(const char *json, const char *key, bool expected);
bool  qmp_json_string(const char *json, const char *key, const char *value);
bool  qmp_status_is_running(const char *status_json);
bool  qmp_status_is_postmigrate(const char *status_json);
bool qmp_status_is_paused(const char *json);
bool qmp_status_is_inmigrate(const char *json);
// qmp_migration_active는 static으로 qmp.c 안에서만 쓰면 헤더 선언 불필요


/* 보조 */
int   qmp_ensure_running(int qmp_fd, const char *context);
void  qmp_try_log_cancel(int qmp_fd, const char *context);

/* 기타 I/O 유틸이 qmp.c에 있다면 필요에 따라 노출 */
int   set_timeouts(int fd, int r_ms, int w_ms);
void  sleep_ms(unsigned int ms);

// ===== line I/O =====
void   init_read_buffer(ReadBuffer *rb);
char * read_line_buffered(int fd, ReadBuffer *rb);  // 한 줄(개행 포함) 수신 → NUL종료 문자열 반환(호출자가 free)
char * read_resp_line(int fd, ReadBuffer *rb);      // 빈 줄/배너 건너뛰고 JSON 라인 수신
int    send_line(int fd, const char *json);         // line + '\n' 송신, 오류 시 -1

int wait_for_migration_complete(int qmp_fd);



