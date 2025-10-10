#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef READ_BUFSZ
#define READ_BUFSZ 65536
#endif

typedef struct {
    int fd;                 // write target (pipe write end)
    char *path;             // input gz path (heap owned)
    volatile int error;
    char errmsg[256];
    pthread_mutex_t mutex;
    volatile int cancel;
    size_t bytes_processed;
} GzipThreadCtx;

void *gzip_source_thread(void *opaque);
int   compress_raw_snapshot(const char *raw_path, const char *gz_path);

bool  pigz_available(void);
int   pigz_thread_count(void);
