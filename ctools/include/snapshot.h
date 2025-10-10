#pragma once
#include <stdbool.h>
#include <stddef.h>

/* 경로/이름 유틸 */
char *snapshot_path_from_name(const char *name);  // malloc 반환, free 필요
void  sanitize_snapshot_name(const char *name, char *out, size_t out_sz);

/* 스냅샷 동작 */
int save_snapshot_gz(const char *outfile, const char *hmp_name, bool create_internal);
int load_snapshot_gz(const char *infile);
int load_snapshot_internal(const char *snap_name);
int delete_snapshot(const char *name);
int list_snapshots(void);
