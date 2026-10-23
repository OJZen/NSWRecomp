// Header validation adapted from Zelda-Mobile host/services.c; Apache-2.0,
// see third_party/zelda-mobile/LICENSE. Host storage is strictly read-only.
#include "romfs.h"
#include <stdlib.h>
#include <limits.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct Romfs {
    uint64_t size;
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

uint64_t romfs_size(const Romfs *storage) { return storage ? storage->size : 0; }

void romfs_close(Romfs *storage) {
    if (!storage) return;
#ifdef _WIN32
    CloseHandle(storage->handle);
#else
    close(storage->fd);
#endif
    free(storage);
}

bool romfs_read(Romfs *storage, uint64_t offset, void *destination, size_t size) {
    if (!storage || offset > storage->size || size > storage->size - offset || (size && !destination)) return false;
#ifdef _WIN32
    LARGE_INTEGER position; position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(storage->handle, position, NULL, FILE_BEGIN)) return false;
#endif
    for (size_t done = 0; done < size;) {
        size_t count = size - done < 65536 ? size - done : 65536;
#ifdef _WIN32
        DWORD got;
        if (!ReadFile(storage->handle, (char *)destination + done, (DWORD)count, &got, NULL) || !got) return false;
#else
        ssize_t got = pread(storage->fd, (char *)destination + done, count, (off_t)(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
#endif
        done += (size_t)got;
    }
    return true;
}

Romfs *romfs_open(const char *path, uintptr_t inherited_handle) {
    Romfs *storage = calloc(1, sizeof *storage);
    if (!storage) return NULL;
#ifdef _WIN32
    storage->handle = INVALID_HANDLE_VALUE;
    if (inherited_handle) {
        // The launcher holds this exact verified file open without write/delete
        // sharing. Duplicating it avoids a path lookup between hash and use.
        if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)inherited_handle, GetCurrentProcess(),
                             &storage->handle, GENERIC_READ, FALSE, 0)) goto fail;
    } else {
        if (!path || !*path) goto fail;
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
        if (!length) goto fail;
        wchar_t *wide = malloc((size_t)length * sizeof *wide);
        if (!wide) goto fail;
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, length))
            storage->handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                          FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        free(wide);
    }
    BY_HANDLE_FILE_INFORMATION info; LARGE_INTEGER size;
    if (storage->handle == INVALID_HANDLE_VALUE || GetFileType(storage->handle) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(storage->handle, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !GetFileSizeEx(storage->handle, &size) || size.QuadPart < 0x50) goto fail;
    storage->size = (uint64_t)size.QuadPart;
#else
    storage->fd = -1;
    if (inherited_handle || !path || !*path) goto fail;
    storage->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    struct stat info;
    if (storage->fd < 0 || fstat(storage->fd, &info) || !S_ISREG(info.st_mode) || info.st_size < 0x50) goto fail;
    storage->size = (uint64_t)info.st_size;
#endif
    uint64_t header[10];
    if (!romfs_read(storage, 0, header, sizeof header) || header[0] != 0x50) goto fail;
    for (unsigned i = 1; i < 9; i += 2) {
        if (header[i] < 0x50 || header[i] > storage->size || header[i + 1] > storage->size - header[i]) goto fail;
        for (unsigned j = 1; j < i; j += 2)
            if (header[i] < header[j] + header[j + 1] && header[j] < header[i] + header[i + 1]) goto fail;
    }
    if (header[4] < 24 || header[9] < 0x50 || header[9] > storage->size) goto fail;
    return storage;
fail:
    romfs_close(storage);
    return NULL;
}
