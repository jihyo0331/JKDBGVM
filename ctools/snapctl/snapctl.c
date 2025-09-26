#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#define READ_BUFSZ 65536
#define WRITE_TIMEOUT_MS 1000   
#define READ_TIMEOUT_MS  2000  
#define MAX_RETRY_COUNT 5       

static const char *g_sock_path = NULL;

/* ---------- util ---------- */
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(EXIT_FAILURE);
}

static int set_timeouts(int fd, int r_ms, int w_ms) {
    struct timeval r = { .tv_sec = r_ms/1000, .tv_usec = (r_ms%1000)*1000 };
    struct timeval w = { .tv_sec = w_ms/1000, .tv_usec = (w_ms%1000)*1000 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof(r)) < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &w, sizeof(w)) < 0) return -1;
    return 0;
}

// 버퍼링된 읽기로 성능 개선
static char *read_line_buffered(int fd) {
    static char buffer[READ_BUFSZ];
    static char *buf_ptr = NULL;
    static size_t buf_len = 0;
    static char line_buf[READ_BUFSZ];
    
    size_t line_pos = 0;
    
    while (line_pos < sizeof(line_buf) - 1) {
        // 버퍼가 비어있으면 새로 읽기
        if (buf_len == 0) {
            ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                if (n == 0 && line_pos > 0) break;
                if (n < 0 && errno == EINTR) continue;
                return NULL;
            }
            buf_ptr = buffer;
            buf_len = n;
        }
        
        // 버퍼에서 한 문자씩 처리
        char c = *buf_ptr++;
        buf_len--;
        line_buf[line_pos++] = c;
        
        if (c == '\n') break;
    }
    
    line_buf[line_pos] = '\0';
    return line_pos > 0 ? strdup(line_buf) : NULL;
}

static char *read_resp_line(int fd) {
    for (;;) {
        char *line = read_line_buffered(fd);
        if (!line) return NULL;
        // 이벤트 메시지는 건너뛰기
        if (strstr(line, "\"event\"")) { 
            free(line); 
            continue; 
        }
        return line;
    }
}

static int send_line(int fd, const char *json) {
    size_t len = strlen(json);
    // 한 번에 전송하도록 개선
    char *buf = malloc(len + 2);
    if (!buf) return -1;
    
    memcpy(buf, json, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';
    
    ssize_t sent = send(fd, buf, len + 1, 0);
    free(buf);
    
    return (sent == (ssize_t)(len + 1)) ? 0 : -1;
}

static bool looks_like_qmp_error(const char *resp) {
    if (!resp) return true;
    return strstr(resp, "\"error\"") != NULL;
}

/* ---------- QMP 연결 & 네고 ---------- */
static int qmp_connect(const char *sockpath) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) die("socket: %s", strerror(errno));
    
    // 논블로킹으로 설정하여 빠른 연결 실패 감지
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(sockpath) >= sizeof(addr.sun_path))
        die("socket path too long");
    strcpy(addr.sun_path, sockpath);
    
    int ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        close(fd);
        die("connect(%s): %s", sockpath, strerror(errno));
    }
    
    // 다시 블로킹으로 설정
    fcntl(fd, F_SETFL, flags);
    
    if (set_timeouts(fd, READ_TIMEOUT_MS, WRITE_TIMEOUT_MS) < 0) {
        close(fd);
        die("setsockopt timeouts: %s", strerror(errno));
    }
    return fd;
}

static int qmp_open_and_negotiate(void) {
    int fd = qmp_connect(g_sock_path);
    char *banner = read_line_buffered(fd);
    if (!banner) { close(fd); die("read banner failed"); }
    free(banner);
    
    if (send_line(fd, "{\"execute\":\"qmp_capabilities\"}") < 0) {
        close(fd); die("write capabilities failed: %s", strerror(errno));
    }
    
    char *neg = read_resp_line(fd);
    if (!neg) { close(fd); die("read capabilities failed"); }
    free(neg);
    return fd;
}

/* ---------- QMP/HMP 명령 ---------- */
static char *qmp_cmd(int fd, const char *fmt, ...) {
    char *payload = NULL;
    va_list ap; va_start(ap, fmt);
    if (vasprintf(&payload, fmt, ap) < 0) payload = NULL;
    va_end(ap);
    if (!payload) die("OOM");
    
    if (send_line(fd, payload) < 0) { 
        free(payload); 
        return NULL; 
    }
    free(payload);
    return read_resp_line(fd);
}

static char *qmp_exec_simple(const char *json) {
    int fd = qmp_open_and_negotiate();
    if (send_line(fd, json) < 0) { close(fd); return NULL; }
    char *resp = read_resp_line(fd);
    close(fd);
    return resp;
}

static char *qmp_hmp_passthru(const char *hmp) {
    int fd = qmp_open_and_negotiate();
    char *resp = NULL;
    char *payload = NULL;
    
    if (asprintf(&payload,
                 "{\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":\"%s\"}}",
                 hmp) < 0) payload = NULL;
    if (!payload) { close(fd); return NULL; }
    
    if (send_line(fd, payload) == 0)
        resp = read_resp_line(fd);
    free(payload);
    close(fd);
    return resp;
}

/* ---------- 상태 확인 및 resume 보정 (개선됨) ---------- */
static bool looks_running(const char *resp) {
    if (!resp) return false;
    return strstr(resp, "\"status\":\"running\"") || strstr(resp, "\"running\":true");
}

static bool qmp_is_running(void) {
    char *resp = qmp_exec_simple("{\"execute\":\"query-status\"}");
    bool ok = looks_running(resp);
    free(resp);
    return ok;
}

// 더 적극적인 resume 로직으로 paused 문제 해결
static int qmp_ensure_running(void) {
    for (int i = 0; i < MAX_RETRY_COUNT; i++) {
        if (qmp_is_running()) return 0;
        
        // 점진적으로 대기 시간 증가
        struct timespec ts = { 
            .tv_sec = 0, 
            .tv_nsec = (50 + i * 50) * 1000 * 1000  // 50ms, 100ms, 150ms...
        };
        nanosleep(&ts, NULL);
        
        // cont 명령 여러 번 시도
        for (int j = 0; j < 3; j++) {
            char *r = qmp_exec_simple("{\"execute\":\"cont\"}");
            if (r && !looks_like_qmp_error(r)) {
                free(r);
                break;
            }
            free(r);
            
            // HMP로도 시도
            r = qmp_hmp_passthru("cont");
            free(r);
            
            struct timespec short_wait = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
            nanosleep(&short_wait, NULL);
        }
        
        // 최종 확인
        if (qmp_is_running()) return 0;
    }
    
    fprintf(stderr, "Warning: VM may still be paused after %d attempts\n", MAX_RETRY_COUNT);
    return -1;
}

/* ---------- 스냅샷 API (개선됨) ---------- */
static int save_snapshot(const char *name) {
    // QMP 우선 시도
    char json[1024];
    snprintf(json, sizeof(json), "{\"execute\":\"savevm\",\"arguments\":{\"name\":\"%s\"}}", name);
    char *resp = qmp_exec_simple(json);
    
    if (resp && !looks_like_qmp_error(resp)) {
        free(resp);
        return 0;
    }
    free(resp);
    
    // HMP 폴백
    char hmp[1024];
    snprintf(hmp, sizeof(hmp), "savevm %s", name);
    resp = qmp_hmp_passthru(hmp);
    bool ok = (resp && strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL);
    free(resp);
    
    return ok ? 0 : -1;
}

static int load_snapshot(const char *name) {
    int fd = qmp_open_and_negotiate();
    bool success = false;
    
    // Step 1: Stop VM
    char *r1 = qmp_cmd(fd, "{\"execute\":\"stop\"}");
    if (r1 && !looks_like_qmp_error(r1)) {
        free(r1);
        
        // Step 2: Load snapshot
        char *r2 = qmp_cmd(fd, "{\"execute\":\"loadvm\",\"arguments\":{\"name\":\"%s\"}}", name);
        if (r2 && !looks_like_qmp_error(r2)) {
            free(r2);
            
            // Step 3: Resume immediately
            char *r3 = qmp_cmd(fd, "{\"execute\":\"cont\"}");
            if (r3 && !looks_like_qmp_error(r3)) {
                success = true;
            }
            free(r3);
        } else {
            free(r2);
        }
    } else {
        free(r1);
    }
    
    close(fd);
    
    // QMP가 실패했다면 HMP로 시도
    if (!success) {
        char *s1 = qmp_hmp_passthru("stop");
        if (s1) {
            free(s1);
            
            char hmp[1024];
            snprintf(hmp, sizeof(hmp), "loadvm %s", name);
            char *s2 = qmp_hmp_passthru(hmp);
            if (s2) {
                bool load_ok = (strstr(s2, "error") == NULL && strstr(s2, "Error") == NULL);
                free(s2);
                
                if (load_ok) {
                    char *s3 = qmp_hmp_passthru("cont");
                    if (s3) {
                        success = (strstr(s3, "error") == NULL);
                        free(s3);
                    }
                }
            }
        }
    }
    
    // 마지막으로 VM이 정상 실행되는지 확인하고 강제로 resume
    if (qmp_ensure_running() == 0) {
        return 0;
    }
    
    return success ? 0 : -1;
}

static int delete_snapshot(const char *name) {
    char json[1024];
    snprintf(json, sizeof(json), "{\"execute\":\"delvm\",\"arguments\":{\"name\":\"%s\"}}", name);
    char *resp = qmp_exec_simple(json);
    
    if (resp && !looks_like_qmp_error(resp)) { 
        free(resp); 
        return 0; 
    }
    free(resp);
    
    char hmp[1024];
    snprintf(hmp, sizeof(hmp), "delvm %s", name);
    resp = qmp_hmp_passthru(hmp);
    bool ok = (resp && strstr(resp, "error") == NULL && strstr(resp, "Error") == NULL);
    free(resp);
    return ok ? 0 : -1;
}

static int list_snapshots(void) {
    char *resp = qmp_exec_simple("{\"execute\":\"query-savevm\"}");
    if (resp && !looks_like_qmp_error(resp)) {
        printf("%s", resp);
        free(resp);
        return 0;
    }
    free(resp);
    
    resp = qmp_hmp_passthru("info snapshots");
    if (!resp) return -1;
    printf("%s", resp);
    free(resp);
    return 0;
}

/* ---------- CLI ---------- */
static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --socket <path> <cmd> [name]\n"
        "  cmds:\n"
        "    savevm <name>\n"
        "    loadvm <name>\n"
        "    delvm  <name>\n"
        "    list\n", prog);
}

int main(int argc, char **argv) {
    const char *socket_arg = NULL;
    int i = 1;
    
    while (i < argc) {
        if (!strcmp(argv[i], "--socket") && i+1 < argc) {
            socket_arg = argv[++i];
        } else break;
        i++;
    }
    
    if (!socket_arg) {
        const char *home = getenv("HOME");
        static char def[512];
        if (!home) die("HOME not set and --socket not provided");
        snprintf(def, sizeof(def), "%s/vm/win11/qmp.sock", home);
        g_sock_path = def;
    } else {
        g_sock_path = socket_arg;
    }

    if (i >= argc) { usage(argv[0]); return 1; }
    const char *cmd = argv[i++];

    if (!strcmp(cmd, "savevm")) {
        if (i >= argc) { usage(argv[0]); return 1; }
        return save_snapshot(argv[i]) == 0 ? 0 : 2;
    } else if (!strcmp(cmd, "loadvm")) {
        if (i >= argc) { usage(argv[0]); return 1; }
        return load_snapshot(argv[i]) == 0 ? 0 : 2;
    } else if (!strcmp(cmd, "delvm")) {
        if (i >= argc) { usage(argv[0]); return 1; }
        return delete_snapshot(argv[i]) == 0 ? 0 : 2;
    } else if (!strcmp(cmd, "list")) {
        return list_snapshots() == 0 ? 0 : 2;
    } else {
        usage(argv[0]); return 1;
    }
}