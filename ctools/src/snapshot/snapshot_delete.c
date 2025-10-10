#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "snapshot.h"
#include "qmp.h"
#include "hmp.h"
#include "gzip_util.h"
#include "timelog.h"
#include "snapctl.h"

/* 내부+외부 스냅샷 삭제 */
int delete_snapshot(const char *name) {
    if (!name) return -1;

    int rc = 0;

    /* 내부 스냅샷 삭제 (있다면) */
    char sanitized[SNAP_NAME_MAX_LEN];
    sanitize_snapshot_name(name, sanitized, sizeof(sanitized));

    ReadBuffer rb;
    int qfd = qmp_open_and_negotiate(&rb);
    if (qfd >= 0) {
        if (hmp_delete_snapshot(qfd, sanitized) < 0) {
            rc = -1; /* 내부 스냅샷이 없으면 실패할 수 있으니 rc를 -1로만 표시 */
        }
        close(qfd);
    } else {
        timing_log(LOG_WARN, "QMP 연결 실패(내부 스냅샷 삭제는 건너뜀)");
    }

    /* 외부 gzip 파일 삭제 */
    char *path = snapshot_path_from_name(name);
    if (path) {
        if (unlink(path) != 0 && errno != ENOENT) {
            timing_log(LOG_ERROR, "%s 삭제 실패: %s", path, strerror(errno));
            rc = -1;
        }
        free(path);
    }
    return rc;
}
