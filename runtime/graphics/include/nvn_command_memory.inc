#define HOSTSEG_SZ (1u << 20)
#define HOSTSEG_MAX 64
// shortcut: overflow records live until process exit; add handle ownership before reclaiming them.
static struct { uint8_t *mem; size_t used; } g_hostseg[HOSTSEG_MAX];
static pthread_mutex_t g_hostseg_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *hostseg_take(size_t size) {
    if (!size || size > HOSTSEG_SZ || (size & 7)) die("NVN overflow allocation size");
    uint8_t *memory = NULL;
    pthread_mutex_lock(&g_hostseg_mu);
    for (unsigned i = 0; i < HOSTSEG_MAX; i++) if (size <= HOSTSEG_SZ - g_hostseg[i].used) {
        if (!g_hostseg[i].mem) g_hostseg[i].mem = malloc(HOSTSEG_SZ);
        if (!g_hostseg[i].mem) break;
        memory = g_hostseg[i].mem + g_hostseg[i].used;
        g_hostseg[i].used += size;
        break;
    }
    pthread_mutex_unlock(&g_hostseg_mu);
    if (!memory) die("NVN overflow storage exhausted (64 MiB lifetime limit)");
    return memory;
}
static void cmd_host_segment(cmd_t *o, size_t size) {
    // The game advances its arena by GetCommandMemoryUsed; host spill bytes must not replace that count.
    if (o->guest_active) o->guest_used = o->cur - o->guest_memory + (o->recording && o->begin ? 16 : 0);
    o->guest_active = 0;
    cmd_new_segment(o, hostseg_take(size), size);
}
