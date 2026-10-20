#pragma once
#include <stdbool.h>
#include <stdint.h>

// A request starts at the next translated frame boundary, never in the middle of a frame.
bool zelda_resource_journal_request(const char *path);

#ifdef ZELDA_RESOURCE_JOURNAL_IMPLEMENTATION
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { ZJ_LIMIT = 16384 };
enum { ZJ_TEXTURE_BIND = 1, ZJ_BUFFER_BIND, ZJ_ATTACHMENT, ZJ_UPLOAD,
       ZJ_COPY_READ, ZJ_COPY_WRITE, ZJ_COMMIT, ZJ_OPERATION, ZJ_GAP, ZJ_ARGS_BEGIN, ZJ_ARGS_END, ZJ_ARGS_REUSE,
       ZJ_DRAW, ZJ_DISPATCH, ZJ_PIPELINE };
enum { ZJ_READ = 1, ZJ_MAY_WRITE = 2, ZJ_CLEAR = 4 };
enum { ZJ_SEQ, ZJ_FRAME, ZJ_SUBMIT, ZJ_PASS, ZJ_KIND, ZJ_ACCESS, ZJ_SERIAL, ZJ_ADDRESS,
       ZJ_SIZE, ZJ_GENERATION, ZJ_RESOURCE, ZJ_STORAGE, ZJ_HEAP, ZJ_HEAP_OFFSET,
       ZJ_BUFFER, ZJ_OFFSET, ZJ_STORAGE_BYTES, ZJ_LEVEL, ZJ_LEVELS, ZJ_LAYER, ZJ_LAYERS, ZJ_TYPE,
       ZJ_FORMAT, ZJ_WIDTH, ZJ_HEIGHT, ZJ_DEPTH, ZJ_SAMPLES, ZJ_SWIZZLE,
       ZJ_EXPECTED_TYPE, ZJ_STAGE, ZJ_REG, ZJ_DIRTY_EPOCH, ZJ_FENCE, ZJ_DETAIL,
       ZJ_ENCODER, ZJ_COMPLETED, ZJ_COMPLETION_ERROR, ZJ_LENGTH, ZJ_X, ZJ_Y, ZJ_Z, ZJ_NX, ZJ_NY, ZJ_NZ, ZJ_FIELDS };
static const char zj_columns[] = "seq\tframe\tsubmit\tpass\tkind\taccess\tserial\taddress\tsize\tgeneration\tresource\tstorage\theap\theap_offset\tbuffer\toffset\tstorage_bytes\tlevel\tlevels\tlayer\tlayers\ttype\tformat\twidth\theight\tdepth\tsamples\tswizzle\texpected_type\tstage\treg\tdirty_epoch\tfence\tdetail\tencoder\tcompleted\tcompletion_error\tlength\tx\ty\tz\tnx\tny\tnz\tshader";
typedef struct { uint64_t v[ZJ_FIELDS]; char shader[40]; } zj_event;
static pthread_mutex_t zj_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic bool zj_active;
static _Atomic bool zj_full;
static _Atomic uint64_t zj_completed;
static _Atomic bool zj_completion_error;
static zj_event *zj_events;
static FILE *zj_file;
static char zj_path[1024];
static uint64_t zj_frame, zj_target = UINT64_MAX, zj_dropped;
static unsigned zj_count;
static bool zj_pending, zj_initialized, zj_have_frame;
static uint64_t zj_last_frame;

bool zelda_resource_journal_request(const char *path) {
    if (!path || !*path || strlen(path) >= sizeof zj_path) return false;
    pthread_mutex_lock(&zj_mutex);
    bool ok = !zj_pending && !atomic_load(&zj_active);
    if (ok) { strcpy(zj_path, path); zj_pending = true; zj_target = UINT64_MAX; }
    pthread_mutex_unlock(&zj_mutex);
    return ok;
}
static void zj_finish(void) {
    for (unsigned i = 0; i < zj_count && !ferror(zj_file); i++) {
        for (unsigned j = 0; j < ZJ_FIELDS; j++) fprintf(zj_file, "%" PRIu64 "\t", zj_events[i].v[j]);
        fprintf(zj_file, "%s\n", zj_events[i].shader);
    }
    if (!ferror(zj_file)) fprintf(zj_file, "END\t%u\t%" PRIu64 "\n", zj_count, zj_dropped);
    bool failed = fflush(zj_file) || ferror(zj_file);
    if (fclose(zj_file)) failed = true;
    fprintf(stderr, "RESOURCE_JOURNAL frame=%" PRIu64 " events=%u dropped=%" PRIu64 " saved=%d\n", zj_frame, zj_count, zj_dropped, !failed);
    free(zj_events); zj_events = NULL; zj_file = NULL; atomic_store(&zj_active, false);
}
static void zj_tick(uint64_t frame, const char *contract) {
    pthread_mutex_lock(&zj_mutex);
    bool boundary = !zj_have_frame || frame != zj_last_frame;
    zj_have_frame = true; zj_last_frame = frame;
    if (!zj_initialized) {
        zj_initialized = true;
        const char *path = getenv("ZELDA_RESOURCE_JOURNAL"), *number = getenv("ZELDA_RESOURCE_FRAME");
        if (!zj_pending && path && *path && strlen(path) < sizeof zj_path && number && *number) {
            char *end; errno = 0; unsigned long long target = strtoull(number, &end, 10);
            if (!errno && !*end && strspn(number, "0123456789") == strlen(number)) {
                strcpy(zj_path, path); zj_pending = true; zj_target = target;
            } else fprintf(stderr, "RESOURCE_JOURNAL invalid frame\n");
        }
    }
    if (atomic_load(&zj_active) && frame != zj_frame) zj_finish();
    if (zj_pending && boundary && (zj_target == UINT64_MAX || frame >= zj_target)) {
        zj_pending = false;
        if (zj_target != UINT64_MAX && frame != zj_target) {
            fprintf(stderr, "RESOURCE_JOURNAL requested frame already passed\n");
        } else {
            int fd = open(zj_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
            zj_file = fd >= 0 ? fdopen(fd, "w") : NULL;
            if (!zj_file && fd >= 0) close(fd);
            if (zj_file) zj_events = calloc(ZJ_LIMIT, sizeof *zj_events);
            if (zj_file && zj_events) {
                zj_frame = frame; zj_count = 0; zj_dropped = 0; atomic_store(&zj_full, false);
                fprintf(zj_file, "ZJ2\t%" PRIu64 "\t%d\t%s\n%s\n", frame, ZJ_LIMIT, contract, zj_columns);
                if (!fflush(zj_file)) atomic_store(&zj_active, true);
            }
            if (!atomic_load(&zj_active)) {
                if (zj_file) fclose(zj_file);
                free(zj_events); zj_events = NULL; zj_file = NULL;
                fprintf(stderr, "RESOURCE_JOURNAL could not start capture\n");
            }
        }
    }
    pthread_mutex_unlock(&zj_mutex);
}
static void zj_record(zj_event event) {
    if (!atomic_load(&zj_active)) return;
    pthread_mutex_lock(&zj_mutex);
    if (atomic_load(&zj_active)) {
        if (zj_count == ZJ_LIMIT) zj_dropped++;
        else {
            event.v[ZJ_SEQ] = zj_count + 1; event.v[ZJ_FRAME] = zj_frame;
            event.shader[sizeof event.shader - 1] = 0;
            if (!event.shader[0] || strspn(event.shader, "0123456789abcdef_-") != strlen(event.shader)) strcpy(event.shader, "-");
            zj_events[zj_count++] = event;
            if (zj_count == ZJ_LIMIT) atomic_store(&zj_full, true);
        }
    }
    pthread_mutex_unlock(&zj_mutex);
}
static bool zj_capacity(void) {
    if (!atomic_load(&zj_active)) return false;
    if (!atomic_load(&zj_full)) return true;
    zj_record((zj_event){0}); return false;
}
#endif
