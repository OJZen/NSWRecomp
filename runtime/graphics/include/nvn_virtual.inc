#include <sys/mman.h>
#include <mach/mach.h>
#include "mach_memory.h"
#include <pthread.h>
#include "nvn_dirty.h"

typedef struct ZeldaPoolAlias { pool_t *pool; struct ZeldaPoolAlias *next; } ZeldaPoolAlias;
static ZeldaPoolAlias *z_pool_aliases;
static pthread_mutex_t z_pool_alias_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t nvn_canonical_address(uint64_t address, uint64_t length) {
    if (!address || length > UINT64_MAX - address) return 0;
    uint64_t result = address;
    pthread_mutex_lock(&z_pool_alias_lock);
    for (ZeldaPoolAlias *a = z_pool_aliases; a; a = a->next) {
        uint64_t start = (uintptr_t)a->pool->mem, end = start + a->pool->size;
        if (address < end && address + length > start) {
            result = address >= start && address + length <= end ?
                (uintptr_t)a->pool->physical + address - start : 0;
            break;
        }
    }
    pthread_mutex_unlock(&z_pool_alias_lock);
    return result;
}

typedef struct {
    const pool_t *physical_pool;
    int64_t physical_offset, virtual_offset;
    uint64_t size;
    uint32_t storage_class, reserved;
} ZeldaMappingRequest;
_Static_assert(sizeof(ZeldaMappingRequest) == 40, "NVN mapping request ABI");

static int virtual_pool_initialize(pool_t *pool) {
    if (!pool->size || pool->size % 0x10000) return 0;
    void *address = mmap(NULL, pool->size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (address == MAP_FAILED) return 0;
    pool->mem = address; pool->physical = NULL;
    return 1;
}
static void virtual_pool_finalize(pool_t *pool) {
    pthread_mutex_lock(&z_pool_alias_lock);
    ZeldaPoolAlias **link = &z_pool_aliases;
    while (*link && (*link)->pool != pool) link = &(*link)->next;
    if (*link) { ZeldaPoolAlias *old = *link; *link = old->next; free(old); }
    pthread_mutex_unlock(&z_pool_alias_lock);
    if (pool->physical) { nvn_dirty_remove_range(pool->mem); mtl_pool_del(pool->mem); }
    if (pool->mem) munmap(pool->mem, pool->size);
    pool->physical = NULL;
}
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *object, int count, const NVNmappingRequest *requests) {
    pool_t *pool = (pool_t *)object;
    if (!(pool->flags & 0x200) || !pool->mem || count != 1 || !requests) {
        LOG("BLOCKED NVN virtual mapping request count=%d", count); exit(78);
    }
    const ZeldaMappingRequest *r = (const ZeldaMappingRequest *)requests;
    const pool_t *source = r->physical_pool;
    // shortcut: one contiguous alias per pool; implement sparse/remapped pages if Zelda requests them.
    if (!source || source->flags & 0x200 || r->physical_offset < 0 || r->virtual_offset || r->size != pool->size ||
        (uint64_t)r->physical_offset > source->size || r->size > source->size - (uint64_t)r->physical_offset ||
        ((uintptr_t)source->mem + r->physical_offset) % 0x4000 || !source->mem) {
        LOG("BLOCKED NVN partial virtual mapping offset=%lld length=%llu pool=%llu", r->virtual_offset, r->size, pool->size); exit(78);
    }
    uint8_t *physical = source->mem + r->physical_offset;
    if (pool->physical) {
        if (pool->physical == physical) return 1;
        LOG("BLOCKED NVN virtual pool remapping"); exit(78);
    }
    mach_vm_address_t address = (uintptr_t)pool->mem;
    vm_prot_t current, maximum;
    kern_return_t result = mach_vm_remap(mach_task_self(), &address, pool->size, 0,
        VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (uintptr_t)physical, FALSE,
        &current, &maximum, VM_INHERIT_NONE);
    if (result) return 0;
    pool->physical = physical;
    mtl_pool_add(pool->mem, pool->size);
    // Writes through the alias must mark its textures dirty too (the physical range is tracked by its own pool).
    nvn_dirty_add_range(pool->mem, pool->size);
    ZeldaPoolAlias *alias = malloc(sizeof *alias);
    if (!alias) { LOG("BLOCKED NVN alias metadata allocation"); exit(78); }
    pthread_mutex_lock(&z_pool_alias_lock);
    *alias = (ZeldaPoolAlias){pool, z_pool_aliases}; z_pool_aliases = alias;
    pthread_mutex_unlock(&z_pool_alias_lock);
    OLOG("Virtual memory pool aliased bytes=%llu", pool->size);
    return 1;
}
