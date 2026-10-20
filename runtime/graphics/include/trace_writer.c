#include "trace_writer.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TRACE_CAPACITY = 1 << 20, TRACE_CHUNK = 32768 };
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t worker;
    FILE *file;
    size_t head, tail, used;
    int writing, closing, error;
    char bytes[TRACE_CAPACITY];
} TraceWriter;
static _Atomic(TraceWriter *) active_writer;
static _Atomic(FILE *) active_stream;

static void *trace_worker(void *opaque) {
    TraceWriter *w = opaque; char chunk[TRACE_CHUNK];
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->used && !w->closing) pthread_cond_wait(&w->changed, &w->lock);
        if (!w->used && w->closing) { pthread_mutex_unlock(&w->lock); break; }
        size_t size = w->used < sizeof chunk ? w->used : sizeof chunk;
        size_t first = TRACE_CAPACITY - w->tail; if (first > size) first = size;
        memcpy(chunk, w->bytes + w->tail, first); memcpy(chunk + first, w->bytes, size - first);
        w->tail = (w->tail + size) % TRACE_CAPACITY; w->used -= size; w->writing = 1;
        int failed = w->error;
        pthread_mutex_unlock(&w->lock);
        int error = 0;
        if (!failed && (fwrite(chunk, 1, size, w->file) != size || fflush(w->file))) error = errno ? errno : EIO;
        pthread_mutex_lock(&w->lock);
        int report = error && !w->error; if (report) w->error = error;
        w->writing = 0; pthread_cond_broadcast(&w->changed);
        pthread_mutex_unlock(&w->lock);
        if (report) fprintf(stderr, "TRACE_ERROR write error=%d; measurement invalid\n", error);
    }
    if (fclose(w->file) && !w->error) w->error = errno ? errno : EIO;
    return NULL;
}
static int trace_write(void *opaque, const char *bytes, int size) {
    TraceWriter *w = opaque;
    pthread_mutex_lock(&w->lock);
    int report = !w->error && ((size_t)size > TRACE_CAPACITY - w->used);
    if (report) w->error = ENOBUFS;
    int error = w->error;
    if (!error) {
        size_t first = TRACE_CAPACITY - w->head; if (first > (size_t)size) first = (size_t)size;
        memcpy(w->bytes + w->head, bytes, first); memcpy(w->bytes, bytes + first, (size_t)size - first);
        w->head = (w->head + (size_t)size) % TRACE_CAPACITY; w->used += (size_t)size;
        pthread_cond_broadcast(&w->changed);
    }
    pthread_mutex_unlock(&w->lock);
    if (report) fprintf(stderr, "TRACE_ERROR queue full; measurement invalid\n");
    if (error) { errno = error; return -1; }
    return size;
}
static int trace_close(void *opaque) {
    TraceWriter *w = opaque;
    pthread_mutex_lock(&w->lock); w->closing = 1; pthread_cond_broadcast(&w->changed); pthread_mutex_unlock(&w->lock);
    pthread_join(w->worker, NULL);
    int error = w->error;
    atomic_store(&active_writer, NULL); atomic_store(&active_stream, NULL);
    pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); free(w);
    if (error) { errno = error; return -1; }
    return 0;
}
static FILE *trace_wrap(FILE *sink) {
    TraceWriter *w = calloc(1, sizeof *w);
    if (!w) { fclose(sink); return NULL; }
    w->file = sink;
    int error = pthread_mutex_init(&w->lock, NULL);
    if (error) { fclose(sink); free(w); errno = error; return NULL; }
    error = pthread_cond_init(&w->changed, NULL);
    if (error) { pthread_mutex_destroy(&w->lock); fclose(sink); free(w); errno = error; return NULL; }
    error = pthread_create(&w->worker, NULL, trace_worker, w);
    if (error) { pthread_cond_destroy(&w->changed); pthread_mutex_destroy(&w->lock); fclose(sink); free(w); errno = error; return NULL; }
    FILE *stream = funopen(w, NULL, trace_write, NULL, trace_close);
    if (!stream) { int saved = errno; trace_close(w); errno = saved; return NULL; }
    atomic_store(&active_writer, w); atomic_store(&active_stream, stream);
    return stream;
}
FILE *zelda_trace_open(const char *path) {
    if (atomic_load(&active_stream)) { errno = EBUSY; return NULL; }
    FILE *sink = fopen(path, "w"); if (!sink) return NULL;
    const char *flag = getenv("ZELDA_ASYNC_TRACE");
    if (flag && !atoi(flag)) { atomic_store(&active_stream, sink); return sink; }
    FILE *stream = trace_wrap(sink);
    if (stream) fprintf(stderr, "TRACE async capacity=%u\n", TRACE_CAPACITY);
    return stream;
}
bool zelda_trace_flush(void) {
    FILE *stream = atomic_load(&active_stream); if (!stream) return true;
    if (fflush(stream)) return false;
    TraceWriter *w = atomic_load(&active_writer); if (!w) return true;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline); deadline.tv_sec += 5;
    pthread_mutex_lock(&w->lock); int timed_out = 0;
    while ((w->used || w->writing) && !w->error && !timed_out)
        timed_out = pthread_cond_timedwait(&w->changed, &w->lock, &deadline);
    int error = w->error ? w->error : timed_out;
    pthread_mutex_unlock(&w->lock);
    if (error) { errno = error; return false; }
    return true;
}
int zelda_trace_close(void) {
    FILE *stream = atomic_exchange(&active_stream, NULL); return stream ? fclose(stream) : 0;
}
