#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>         
#include <string.h>      
#include <unistd.h>        
#include <sys/types.h>  
#include <sys/socket.h> 
#include <stdlib.h>   
#include <sys/uio.h>

#include "snapshot.h"
#include "qmp.h"
#include "hmp.h"
#include "gzip_util.h"
#include "timelog.h"
#include "snapctl.h"


void sleep_ms(unsigned int ms) {
    struct timespec req = { .tv_sec = ms/1000, .tv_nsec = (long)(ms%1000)*1000000L };
    while (nanosleep(&req, &req) == -1 && errno == EINTR) { /* retry */ }
}

void init_read_buffer(ReadBuffer *rb) {
    rb->buf_ptr = rb->buffer;
    rb->buf_len = 0;
}

char *read_line_buffered(int fd, ReadBuffer *rb) {
    char line_buf[READ_BUFSZ];
    size_t line_pos = 0;

    while (line_pos < sizeof(line_buf) - 1) {
        if (rb->buf_len == 0) {
            ssize_t n = recv(fd, rb->buffer, sizeof(rb->buffer), 0);
            if (n <= 0) {
                if (n == 0 && line_pos > 0) break;
                if (n < 0 && errno == EINTR) continue;
                return NULL;
            }
            rb->buf_ptr = rb->buffer;
            rb->buf_len = (size_t)n;
        }
        char c = *rb->buf_ptr++;
        rb->buf_len--;
        line_buf[line_pos++] = c;
        if (c == '\n') break;
    }
    line_buf[line_pos] = '\0';
    return (line_pos > 0) ? strdup(line_buf) : NULL;
}

char *read_resp_line(int fd, ReadBuffer *rb) {
    for (;;) {
        char *line = read_line_buffered(fd, rb);
        if (!line) return NULL;
        if (strstr(line, "\"event\"")) { free(line); continue; } // skip QMP events
        return line;
    }
}

int send_line(int fd, const char *json) {
    size_t len = strlen(json);
    struct iovec iov[2] = {
        { .iov_base = (void*)json, .iov_len = len },
        { .iov_base = (void*)"\n", .iov_len = 1 }
    };
    ssize_t sent = writev(fd, iov, 2);
    return (sent == (ssize_t)(len + 1)) ? 0 : -1;
}
