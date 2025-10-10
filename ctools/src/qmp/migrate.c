#include <stdio.h>
#include <stdlib.h> 
#include <stdbool.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <unistd.h>

#include "qmp.h"

int wait_for_migration_complete(int qfd) {
    struct timeval old_tv = {0}, big_tv = { .tv_sec = 3600, .tv_usec = 0 };
    socklen_t optlen = sizeof(old_tv);
    getsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &old_tv, &optlen);
    setsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &big_tv, sizeof(big_tv));

    int retry = 0, ticks = 0;
    while (1) {
        char *resp = qmp_cmd(qfd, "{\"execute\":\"query-migrate\"}");
        if (!resp) {
            if (++retry > 50) {
                setsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
                return -1;
            }
            sleep_ms(500);
            continue;
        }

        retry = 0;
        ++ticks;
        bool done = qmp_json_string(resp, "status", "completed");
        bool fail = qmp_json_string(resp, "status", "failed") ||
                    qmp_json_string(resp, "status", "cancelled");

        if (done) printf("[INFO] 마이그레이션 완료\n");
        if (fail) printf("[ERROR] 마이그레이션 실패\n");

        free(resp);

        if (done || fail) {
            setsockopt(qfd, SOL_SOCKET, SO_RCVTIMEO, &old_tv, sizeof(old_tv));
            return done ? 0 : -1;
        }
        sleep_ms(200);
    }
}
