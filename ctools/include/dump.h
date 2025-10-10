#pragma once
#include <pthread.h>
#include <stddef.h>

typedef struct {
    int pipe_fd;
    int out_fd;
    const char *path;
    volatile int error;
    char errmsg[256];
    pthread_mutex_t mutex;
    volatile int cancel;
    size_t bytes_processed;
} DumpThreadCtx;

typedef struct DumpWriter DumpWriter; // opaque

// 시작: 내부적으로 스레드 생성해서 pipe_fd→out_fd로 펌프
int dump_writer_start(int pipe_fd, int out_fd, const char *path,
                      DumpWriter **out_dw, pthread_t *out_thread);

// 조인(+타임아웃 초). 에러코드/메시지/처리바이트 수거
int dump_writer_join(DumpWriter *dw, pthread_t th, int timeout_sec,
                     int *out_error, char out_errmsg[256], size_t *out_bytes);

// 자원 해제(조인 이후 호출)
void dump_writer_destroy(DumpWriter *dw);
