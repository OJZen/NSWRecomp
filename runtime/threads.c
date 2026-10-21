// Synchronization follows d2r-mobile's MIT hle_thread.c; notice: third_party/zelda-mobile/d2r/LICENSE.
// Recompiled threads keep guest registers/stacks separate from their pthread stack.
#include "threads.h"
#include "dispatch.h"
#include "kernel.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { EVENT = 1, THREAD, NONE = 0, MUTEX = 1, CONDITION, ADDRESS, SYNC, SLEEP };
enum { BAD_HANDLE = 0xe401, BAD_STATE = 0xfa01, TIMEOUT = 0xea01, CANCELLED = 0xec01,
       BAD_ADDRESS = 0xcc01, BAD_COMBINATION = 0xe801, NO_RESOURCE = 0xce01 };
#define WAITERS 0x40000000u

typedef struct { unsigned refs, type; bool signaled; } Object;
typedef struct Thread {
    Object object;
    Cpu cpu;
    pthread_t host;
    pthread_cond_t changed;
    jmp_buf unwind;
    uint64_t entry, identifier, mask, wait_address, key, order;
    uint32_t handle, tag, result;
    int priority, core, wait;
    bool started, cancel, can_unwind, main_thread, quiescent;
    struct Thread *next;
} Thread;
static struct { uint32_t id; Object *object; bool writable; } handles[4096];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static Thread *all;
static _Thread_local Thread *current;
static atomic_bool stopping;
static uint32_t next_handle = 0x01000000;
static uint64_t next_identifier = 0x50, wait_order;
static unsigned active;
#ifdef TOTK_TEST_THREADS
extern void (*threads_test_before_wait)(void);
extern void (*threads_test_main_quiescent)(void);
#define WAIT_READY() do { if (threads_test_before_wait) threads_test_before_wait(); } while (0)
#else
#define WAIT_READY() ((void)0)
#endif

static int handle_index(uint32_t id) {
    for (unsigned i = 0; i < 4096; i++) if (handles[i].id == id && id) return (int)i;
    return -1;
}

static uint32_t handle_new(Object *object, bool writable) {
    if (next_handle >= 0x3fffffff) return 0;
    for (unsigned i = 0; i < 4096; i++) if (!handles[i].id) {
        handles[i].id = ++next_handle;
        handles[i].object = object;
        handles[i].writable = writable;
        object->refs++;
        return handles[i].id;
    }
    return 0;
}

static void release(Object *object) {
    if (--object->refs) return;
    if (object->type == THREAD) {
        Thread *thread = (Thread *)object;
        for (Thread **p = &all; *p; p = &(*p)->next)
            if (*p == thread) { *p = thread->next; break; }
        pthread_cond_destroy(&thread->changed);
    }
    free(object);
}

static void close_handle(unsigned i) {
    release(handles[i].object);
    memset(&handles[i], 0, sizeof handles[i]);
}

static Thread *thread_at(uint32_t id) {
    if (id == 0xffff8000u) return current;
    int i = handle_index(id);
    return i >= 0 && handles[i].object->type == THREAD ? (Thread *)handles[i].object : NULL;
}

static uint64_t now(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) abort();
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static int wait_until(pthread_cond_t *condition, int64_t timeout, uint64_t end) {
    if (timeout < 0) return pthread_cond_wait(condition, &lock);
    uint64_t time = now();
    if (time >= end) return ETIMEDOUT;
    uint64_t remaining = end - time;
    struct timespec relative = {(time_t)(remaining / 1000000000), (long)(remaining % 1000000000)};
    return pthread_cond_timedwait_relative_np(condition, &lock, &relative);
}

static void wake(Thread *thread, uint32_t result) {
    thread->wait = NONE;
    thread->result = result;
    pthread_cond_signal(&thread->changed);
}

static void wake_synchronization(void) {
    for (Thread *t = all; t; t = t->next) if (t->wait == SYNC) pthread_cond_signal(&t->changed);
}

static Thread *pick(int kind, uint64_t address) {
    Thread *best = NULL;
    for (Thread *t = all; t; t = t->next)
        if (t->wait == kind && (kind == CONDITION ? t->key : t->wait_address) == address &&
            (!best || t->priority < best->priority || (t->priority == best->priority && t->order < best->order))) best = t;
    return best;
}

static uint32_t block(Thread *thread, int64_t timeout) {
    uint64_t end = timeout > 0 ? now() + (uint64_t)timeout : 0;
    WAIT_READY();
    while (thread->wait != NONE) {
        if (atomic_load(&stopping)) { wake(thread, CANCELLED); break; }
        int result = wait_until(&thread->changed, timeout, end);
        if (result && thread->wait != NONE) wake(thread, result == ETIMEDOUT ? TIMEOUT : NO_RESOURCE);
    }
    return thread->result;
}

static uint32_t read_word(uint64_t address) { return __atomic_load_n((uint32_t *)(uintptr_t)address, __ATOMIC_SEQ_CST); }
static void write_word(uint64_t address, uint32_t value) { __atomic_store_n((uint32_t *)(uintptr_t)address, value, __ATOMIC_SEQ_CST); }
static bool compare_word(uint64_t address, uint32_t *expected, uint32_t value) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)address, expected, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void handoff(uint64_t address) {
    Thread *next = pick(MUTEX, address);
    if (!next) { write_word(address, 0); return; }
    unsigned count = 0;
    for (Thread *t = all; t; t = t->next) count += t->wait == MUTEX && t->wait_address == address;
    write_word(address, next->tag | (count > 1 ? WAITERS : 0));
    wake(next, 0);
}

static void reacquire(Thread *thread) {
    uint32_t value = read_word(thread->wait_address);
    for (;;) {
        if (!value) {
            if (compare_word(thread->wait_address, &value, thread->tag)) { wake(thread, 0); return; }
        } else if (compare_word(thread->wait_address, &value, value | WAITERS)) {
            thread->wait = MUTEX;
            thread->order = ++wait_order;
            return;
        }
    }
}

static Thread *new_thread(void) {
    Thread *thread = calloc(1, sizeof *thread);
    if (!thread) return NULL;
    thread->object.type = THREAD;
    if (pthread_cond_init(&thread->changed, NULL)) { free(thread); return NULL; }
    thread->handle = handle_new(&thread->object, false);
    if (!thread->handle) { pthread_cond_destroy(&thread->changed); free(thread); return NULL; }
    thread->identifier = ++next_identifier;
    thread->next = all; all = thread;
    return thread;
}

uint32_t threads_init(void) {
    pthread_mutex_lock(&lock);
    if (current || active || all) abort();
    atomic_store(&stopping, false);
    Thread *thread = new_thread();
    if (!thread) abort();
    thread->started = true; thread->priority = 44; thread->core = 0; thread->mask = 7;
    thread->main_thread = true;
    thread->host = pthread_self();
    thread->object.refs++; // The running main thread outlives a closed public handle.
    current = thread;
    pthread_mutex_unlock(&lock);
    return thread->handle;
}

void threads_checkpoint(void) {
    if (!current || !atomic_load(&stopping)) return;
    if (current->can_unwind) longjmp(current->unwind, 1);
    // Keep the main worker's image/TLS stack alive until process exit, without accessing services during teardown.
    pthread_mutex_lock(&lock);
    current->quiescent = true; pthread_cond_broadcast(&idle);
#ifdef TOTK_TEST_THREADS
    if (threads_test_main_quiescent) threads_test_main_quiescent();
#endif
    while (atomic_load(&stopping)) pthread_cond_wait(&current->changed, &lock);
    current->quiescent = false;
    pthread_mutex_unlock(&lock);
}

static void *thread_main(void *argument) {
    Thread *thread = argument;
    current = thread;
    if (!setjmp(thread->unwind)) {
        thread->can_unwind = true;
        threads_checkpoint();
        rt_dispatch(&thread->cpu);
        fputs("STARTUP guest thread returned without ExitThread\n", stderr);
        exit(78);
    }
    thread->can_unwind = false;
    pthread_mutex_lock(&lock);
    thread->object.signaled = true;
    wake_synchronization();
    current = NULL;
    release(&thread->object);
    active--;
    pthread_cond_broadcast(&idle);
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void request_stop(void) {
    atomic_store(&stopping, true);
    for (Thread *t = all; t; t = t->next) pthread_cond_signal(&t->changed);
}
void threads_request_stop(void) {
    pthread_mutex_lock(&lock); request_stop(); pthread_mutex_unlock(&lock);
}
static bool main_running_elsewhere(void) {
    for (Thread *t = all; t; t = t->next)
        if (t->main_thread && t != current && !t->quiescent) return true;
    return false;
}
bool threads_stop(void) {
    pthread_mutex_lock(&lock);
    request_stop();
    uint64_t end = now() + 5000000000ull;
    while (active || main_running_elsewhere()) if (wait_until(&idle, 5000000000ll, end)) break;
    bool stopped = !active && !main_running_elsewhere();
    pthread_mutex_unlock(&lock);
    return stopped;
}

bool threads_destroy(void) {
    if (!threads_stop()) return false;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < 4096; i++) if (handles[i].id) close_handle(i);
    if (current) release(&current->object);
    current = NULL;
    pthread_mutex_unlock(&lock);
    return true;
}

bool kernel_event_create(uint32_t *write, uint32_t *read) {
    Object *event = calloc(1, sizeof *event);
    if (!event) return false;
    event->type = EVENT;
    pthread_mutex_lock(&lock);
    *write = handle_new(event, true);
    *read = *write ? handle_new(event, false) : 0;
    if (!*read) {
        if (*write) close_handle((unsigned)handle_index(*write)); else free(event);
        *write = 0;
    }
    pthread_mutex_unlock(&lock);
    return *read != 0;
}

uint32_t kernel_event_copy(uint32_t handle) {
    pthread_mutex_lock(&lock);
    int i = handle_index(handle);
    uint32_t copy = i >= 0 && !handles[i].writable && handles[i].object->type == EVENT ? handle_new(handles[i].object, false) : 0;
    pthread_mutex_unlock(&lock);
    return copy;
}

static uint32_t wait_objects(uint64_t address, unsigned count, int64_t timeout, uint64_t *index) {
    uint32_t ids[64];
    if (count) memcpy(ids, (void *)(uintptr_t)address, count * 4);
    Object *objects[64]; unsigned held = 0;
    uint32_t result = 0; *index = 0;
    for (unsigned i = 0; i < count; i++) {
        int slot = handle_index(ids[i]);
        Object *object = ids[i] == 0xffff8000u ? &current->object : slot >= 0 ? handles[slot].object : NULL;
        if (!object || (slot >= 0 && handles[slot].writable)) { result = BAD_HANDLE; goto done; }
        object->refs++; objects[held++] = object;
    }
    uint64_t end = timeout > 0 ? now() + (uint64_t)timeout : 0;
    current->wait = SYNC;
    WAIT_READY();
    for (;;) {
        if (current->cancel || atomic_load(&stopping)) { current->cancel = false; result = CANCELLED; break; }
        for (unsigned i = 0; i < count; i++) if (objects[i]->signaled) { *index = i; goto done; }
        int error = wait_until(&current->changed, timeout, end);
        if (error) { result = error == ETIMEDOUT ? TIMEOUT : NO_RESOURCE; break; }
    }
done:
    current->wait = NONE;
    for (unsigned i = 0; i < held; i++) release(objects[i]);
    return result;
}

bool threads_svc(unsigned number, uint64_t *r) {
    if (number == 0x08) {
        if ((r[1] & 3) || !rt_find_function(r[1]) || !kernel_access(r[1], 4, 5) ||
            (r[3] & 15) || r[3] < 16 || !kernel_access(r[3] - 16, 16, 3)) { r[0] = BAD_ADDRESS; return true; }
        int priority = (int)r[4], core = (int)r[5];
        if (priority < 28 || priority >= 60) { r[0] = 0xe001; return true; }
        if (core == -2) core = 0;
        if (core < 0 || core > 2) { r[0] = 0xe201; return true; }
        pthread_mutex_lock(&lock);
        Thread *thread = new_thread();
        uint64_t tls = thread ? kernel_thread_local() : 0;
        if (!tls) {
            if (thread) close_handle((unsigned)handle_index(thread->handle));
            r[0] = NO_RESOURCE;
        } else {
            thread->entry = r[1]; thread->priority = priority; thread->core = core; thread->mask = 1ull << core;
            thread->cpu.pc = r[1]; thread->cpu.x[0] = r[2]; thread->cpu.sp = r[3];
            thread->cpu.tpidrro = tls; thread->cpu.tpidr = tls + 0x1000;
            ST64(tls + 0x1f8, tls + 0x200);
            thread->cpu.excl_addr = UINT64_MAX; thread->cpu.thread = thread;
            r[0] = 0; r[1] = thread->handle;
        }
        pthread_mutex_unlock(&lock);
        return true;
    }
    if (number == 0x09) {
        pthread_mutex_lock(&lock);
        Thread *thread = thread_at((uint32_t)r[0]);
        if (!thread) r[0] = BAD_HANDLE;
        else if (thread->started) r[0] = BAD_STATE;
        else {
            pthread_attr_t attributes;
            if (pthread_attr_init(&attributes)) abort();
            if (pthread_attr_setstacksize(&attributes, 64 << 20) ||
                pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED)) abort();
            thread->object.refs++; thread->started = true; active++;
            if (pthread_create(&thread->host, &attributes, thread_main, thread)) {
                active--; thread->started = false; release(&thread->object); r[0] = NO_RESOURCE;
            } else r[0] = 0;
            pthread_attr_destroy(&attributes);
        }
        pthread_mutex_unlock(&lock);
        return true;
    }
    if (number == 0x0a) {
        if (!current || !current->can_unwind) return false;
        longjmp(current->unwind, 1);
    }
    if (number == 0x0b) {
        if (!current || (int64_t)r[0] < -2) return false;
        if ((int64_t)r[0] > 0) {
            pthread_mutex_lock(&lock); current->wait = SLEEP; block(current, (int64_t)r[0]); pthread_mutex_unlock(&lock);
        } else sched_yield();
        r[0] = 0; return true;
    }
    if (number == 0x10) {
        if (!current) return false;
        pthread_mutex_lock(&lock); r[0] = current->core < 0 ? __builtin_ctzll(current->mask) : current->core; pthread_mutex_unlock(&lock);
        return true;
    }
    if (number == 0x0c || number == 0x0d || number == 0x0e || number == 0x0f || number == 0x19 || number == 0x25) {
        pthread_mutex_lock(&lock);
        Thread *thread = thread_at((uint32_t)r[number == 0x0e ? 2 : number == 0x0c || number == 0x25 ? 1 : 0]);
        uint32_t result = thread ? 0 : BAD_HANDLE;
        if (thread) switch (number) {
            case 0x0c: r[1] = thread->priority; break;
            case 0x25: r[1] = thread->identifier; break;
            case 0x0d: if ((uint32_t)r[1] < 28 || (uint32_t)r[1] >= 60) result = 0xe001; else thread->priority = (int)r[1]; break;
            case 0x0e: r[1] = (uint32_t)thread->core; r[2] = thread->mask; break;
            case 0x19: thread->cancel = true; if (thread->wait == SYNC) pthread_cond_signal(&thread->changed); break;
            case 0x0f: {
                int core = (int)r[1]; uint64_t mask = r[2];
                if (core == -2) { core = 0; mask = 1; }
                if (core == -3) core = thread->core;
                if (mask & ~7ull || core < -1 || core > 2) result = 0xe201;
                else if (!mask || (core >= 0 && !(mask & (1ull << core)))) result = BAD_COMBINATION;
                if (!result) { thread->mask = mask; thread->core = core; }
                break;
            }
        }
        r[0] = result;
        pthread_mutex_unlock(&lock); return true;
    }
    if (number == 0x45) {
        uint32_t write, read;
        if (!kernel_event_create(&write, &read)) r[0] = NO_RESOURCE;
        else { r[0] = 0; r[1] = write; r[2] = read; }
        return true;
    }
    if (number == 0x11 || number == 0x12 || number == 0x16 || number == 0x17) {
        pthread_mutex_lock(&lock);
        int slot = handle_index((uint32_t)r[0]);
        if (slot < 0) {
            pthread_mutex_unlock(&lock);
            if (number == 0x16) return false;
            r[0] = BAD_HANDLE; return true;
        }
        Object *object = handles[slot].object;
        uint32_t result = 0;
        if (number == 0x16) close_handle((unsigned)slot);
        else if (object->type != EVENT) result = number == 0x17 ? BAD_STATE : BAD_HANDLE;
        else if ((number == 0x11 && !handles[slot].writable) || (number == 0x17 && handles[slot].writable)) result = BAD_HANDLE;
        else if (number == 0x17 && !object->signaled) result = BAD_STATE;
        else { object->signaled = number == 0x11; if (object->signaled) wake_synchronization(); }
        r[0] = result;
        pthread_mutex_unlock(&lock); return true;
    }
    if (number == 0x18) {
        if (!current) return false;
        unsigned count = (uint32_t)r[2];
        if (count > 64) { r[0] = 0xca01; return true; }
        if (count && !kernel_access(r[1], count * 4, 1)) { r[0] = 0xe601; return true; }
        pthread_mutex_lock(&lock);
        r[0] = wait_objects(r[1], count, (int64_t)r[3], &r[1]);
        pthread_mutex_unlock(&lock); return true;
    }
    if (number == 0x1a || number == 0x1b || number == 0x1c || number == 0x1d || number == 0x34 || number == 0x35) {
        if (!current) return false;
        uint64_t address = r[number == 0x1a ? 1 : 0];
        if ((address & 3) || !kernel_access(address, 4, 3) ||
            (number == 0x1c && ((r[1] & 3) || !kernel_access(r[1], 4, 3)))) { r[0] = BAD_ADDRESS; return true; }
        if ((number == 0x34 || number == 0x35) && (uint32_t)r[1] > 2) { r[0] = 0xf001; return true; }
        pthread_mutex_lock(&lock);
        uint32_t result = 0;
        if (number == 0x1a) {
            if (read_word(address) == ((uint32_t)r[0] | WAITERS)) {
                current->wait = MUTEX; current->wait_address = address; current->tag = (uint32_t)r[2]; current->order = ++wait_order;
                result = block(current, -1);
            }
        } else if (number == 0x1b) handoff(address);
        else if (number == 0x1c) {
            // Publish the key before releasing the mutex, so a user-space notifier cannot miss this waiter.
            write_word(r[1], 1);
            current->wait = CONDITION; current->key = r[1]; current->wait_address = address;
            current->tag = (uint32_t)r[2]; current->order = ++wait_order;
            handoff(address);
            result = block(current, (int64_t)r[3]);
        } else if (number == 0x1d) {
            int count = (int)r[1];
            for (int i = 0; count <= 0 || i < count; i++) {
                Thread *thread = pick(CONDITION, address); if (!thread) break;
                reacquire(thread);
            }
            if (!pick(CONDITION, address)) write_word(address, 0);
        } else if (number == 0x34) {
            uint32_t value, type = (uint32_t)r[1], expected = (uint32_t)r[2];
            for (;;) {
                value = read_word(address);
                if (type == 2 ? value != expected : (int32_t)value >= (int32_t)expected) { result = BAD_STATE; break; }
                if (type != 1 || compare_word(address, &value, value - 1)) break;
            }
            if (!result) {
                current->wait = ADDRESS; current->wait_address = address; current->order = ++wait_order;
                result = block(current, (int64_t)r[3]);
            }
        } else {
            unsigned waiting = 0, type = (uint32_t)r[1]; int count = (int)r[3];
            for (Thread *t = all; t; t = t->next) waiting += t->wait == ADDRESS && t->wait_address == address;
            if (type) {
                uint32_t expected = (uint32_t)r[2], value = expected + 1;
                if (type == 2) value = count <= 0 ? (waiting ? expected - 1 : expected + 1) :
                    !waiting ? expected + 1 : waiting <= (unsigned)count ? expected - 1 : expected;
                if (!compare_word(address, &expected, value)) result = BAD_STATE;
            }
            if (!result) for (int i = 0; count <= 0 || i < count; i++) {
                Thread *thread = pick(ADDRESS, address); if (!thread) break;
                wake(thread, 0);
            }
        }
        r[0] = result;
        pthread_mutex_unlock(&lock); return true;
    }
    return false;
}
