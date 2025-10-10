#pragma once
#include <stdbool.h>
#include <stddef.h>

#define READ_BUFSZ 65536
#define WRITE_TIMEOUT_MS 2000
#define READ_TIMEOUT_MS 30000
#define RETRY_BACKOFF_MS 200
#define QMP_HANDSHAKE_RETRY 3
#define PIPE_BUFFER_SIZE (1024 * 1024)
#define SNAP_NAME_MAX_LEN 128


/* ===== 전역 설정 ===== */
extern const char *g_sock_path;
extern const char *g_timelog_path;
extern const char *g_snapshot_dir;
extern bool g_block_migration;


/* ===== 공용 유틸 ===== */
void sanitize_snapshot_name(const char *name, char *out, size_t out_sz);
