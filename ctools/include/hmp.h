#pragma once
#include <stdbool.h>

char *hmp_command_raw(int qmp_fd, const char *cmdline);
int   hmp_command_check(int qmp_fd, const char *cmdline);
bool  hmp_response_is_error(const char *resp);
void  hmp_print_return_stdout(const char *resp);

int   hmp_save_snapshot(int qmp_fd, const char *snap_name);
int   hmp_load_snapshot(int qmp_fd, const char *snap_name);
int   hmp_delete_snapshot(int qmp_fd, const char *snap_name);
