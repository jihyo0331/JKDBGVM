#pragma once
#include <stddef.h>
int  ensure_dir_exists(const char *dir);
char* snapshot_path_from_name(const char *name); // malloc 반환
void sanitize_snapshot_name(const char *name, char *out, size_t out_sz);
