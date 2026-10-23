// New temporary roots only; no real saves or game-derived fixtures.
#include "filesystem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint64_t value = 0; local_file *file = NULL;
    assert(!filesystem_mount(0)); assert(!filesystem_mount(0));
    assert(!filesystem_path(0, 7, "/", NULL, 0, NULL, &value) && !value);
    assert(!filesystem_path(0, 2, "/nested", NULL, 0, NULL, NULL));
    assert(!filesystem_path(0, 0, "/nested/save.bin", NULL, 16, NULL, NULL));
    assert(filesystem_path(0, 0, "/nested/save.bin", NULL, 16, NULL, NULL) == 0x402);
    assert(!filesystem_path(0, 8, "/nested/save.bin", NULL, 3, &file, NULL));
    assert(filesystem_commit(0));
    char text[] = "synthetic";
    assert(!filesystem_file(file, 1, 2, text, sizeof text, NULL));
    char copy[32] = {0};
    assert(!filesystem_file(file, 0, 0, copy, sizeof copy, &value) && value == 16);
    assert(!memcmp(copy + 2, text, sizeof text) && !copy[0] && !copy[1] && !copy[15]);
    assert(filesystem_file(file, 1, 16, text, 1, NULL));
    assert(filesystem_file(file, 3, UINT64_MAX, NULL, 0, NULL));
    assert(!filesystem_file(file, 2, 0, NULL, 0, NULL));
    assert(!filesystem_file(file, 3, 32, NULL, 0, NULL));
    assert(!filesystem_file(file, 4, 0, NULL, 0, &value) && value == 32);
    filesystem_close(file);
    assert(!filesystem_commit(0));
    assert(!filesystem_path(0, 5, "/nested/save.bin", "/nested/renamed.bin", 0, NULL, NULL));
    assert(filesystem_path(0, 7, "/nested/save.bin", NULL, 0, NULL, &value) == 0x202);
    assert(filesystem_path(0, 7, "/nested/RENAMED.bin", NULL, 0, NULL, &value) == 0x202);
    assert(!filesystem_path(0, 8, "/nested/renamed.bin", NULL, 1, &file, NULL));
    assert(filesystem_file(file, 1, 0, text, 1, NULL));
    filesystem_unmount(0); filesystem_unmount(0);
    assert(!filesystem_file(file, 0, 2, copy, sizeof text, &value) && !memcmp(copy, text, sizeof text));
    filesystem_close(file);
    assert(!filesystem_mount(0));
    assert(!filesystem_path(0, 1, "/nested/renamed.bin", NULL, 0, NULL, NULL));
    assert(!filesystem_path(0, 3, "/nested", NULL, 0, NULL, NULL));
    const char *bad[] = {"/../outside", "/nested/../x", "relative", "/x:stream", "/x\\y", "/CON", "/nul.txt", "/COM1", "/trailing.", "/trailing ", "/a//b"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) assert(filesystem_path(0, 0, bad[i], NULL, 0, NULL, NULL));
    assert(filesystem_path(0, 0, "/huge", NULL, UINT64_C(129) << 20, NULL, NULL));
    assert(filesystem_path(0, 1, "/", NULL, 0, NULL, NULL));
    assert(!filesystem_mount(1));
    assert(filesystem_path(1, 7, "/nested", NULL, 0, NULL, &value) == 0x202);
    filesystem_unmount(0); filesystem_unmount(1); filesystem_shutdown();
    puts("Windows scratch filesystem checks passed");
    return 0;
}
