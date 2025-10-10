#define _GNU_SOURCE
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "snapshot.h"
#include "qmp.h"
#include "hmp.h"
#include "gzip_util.h"
#include "timelog.h"
#include "snapctl.h"


/* 내부 스냅샷 목록 */
int list_internal_snapshots_local(void) {
    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd < 0) {
        timing_log(LOG_WARN, "내부 스냅샷 목록 조회 실패(QMP 연결 실패)");
        return -1;
    }

    char *resp = hmp_command_raw(qfd, "info snapshots");
    close(qfd);
    if (!resp) {
        timing_log(LOG_WARN, "'info snapshots' 명령 실패");
        return -1;
    }

    if (hmp_response_is_error(resp)) {
        timing_log(LOG_WARN, "'info snapshots' 에러: %s", resp);
        free(resp);
        return -1;
    }

    printf("[internal snapshots]\n");
    hmp_print_return_stdout(resp);
    free(resp);
    return 0;
}

/* .gz 아카이브 목록 */
int list_snapshot_archives_local(void) {
    const char *dir = g_snapshot_dir ? g_snapshot_dir : ".";
    DIR *d = opendir(dir);
    if (!d) {
        timing_log(LOG_ERROR, "%s: 디렉터리 열기 실패: %s", dir, strerror(errno));
        return -1;
    }

    printf("[gzip archives] %s\n", dir);
    int found = 0;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        size_t len = strlen(name);
        if (len >= 3 && strcmp(name + len - 3, ".gz") == 0) {
            printf("%s\n", name);
            found++;
        }
    }
    closedir(d);

    if (!found) {
        printf("(no gzip snapshots in %s)\n", dir);
    }
    return 0;
}

/* 통합 목록 */
int list_snapshots(void) {
    int rc = 0;
    if (list_internal_snapshots_local() != 0) rc = -1;
    if (list_snapshot_archives_local() != 0) rc = -1;
    return rc;
}
