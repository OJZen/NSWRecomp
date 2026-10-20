// Adapted from Zelda-Mobile host/filesystem.c; Apache-2.0, see third_party/zelda-mobile/LICENSE.
#include "filesystem.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>

enum { OK = 0, NOT_FOUND = 0x202, EXISTS = 0x402, FAILED = (5305 << 9) | 2 }; // Fs UnexpectedInLocalFileSystem.
static const char *roots[] = {"work/save", "work/temporary", "work/cache-0", "work/cache-1", "work/cache-2", "work/cache-3",
    "work/cache-4", "work/cache-5", "work/cache-6", "work/cache-7"};
static struct { int root, lock; bool mounted, poisoned; unsigned writers, files, refs; } volumes[sizeof roots / sizeof *roots];
struct local_file { int fd; unsigned volume, mode; };
static uint32_t error(void) { return errno == ENOENT || errno == ENOTDIR ? NOT_FOUND : errno == EEXIST ? EXISTS : FAILED; }

static bool exact_name(int parent, const char *name) {
    if (!strcmp(name, ".")) return true;
    int fd = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return false; }
    bool found = false; struct dirent *entry;
    while ((entry = readdir(dir))) if (!strcmp(entry->d_name, name)) { found = true; break; }
    closedir(dir); if (!found) errno = ENOENT;
    return found;
}

static int remove_tree(int parent, const char *name) {
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    DIR *dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    int status = 0; struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) ||
            (S_ISDIR(st.st_mode) ? remove_tree(fd, entry->d_name) : unlinkat(fd, entry->d_name, 0))) { status = -1; break; }
    }
    closedir(dir);
    return status ? status : unlinkat(parent, name, AT_REMOVEDIR);
}

static void unmount_volume(unsigned volume) {
    if (volumes[volume].mounted) {
        char path[64]; snprintf(path, sizeof path, "%s%s", roots[volume], volume == 1 ? "" : ".work");
        close(volumes[volume].root);
        remove_tree(AT_FDCWD, path);
        close(volumes[volume].lock); memset(&volumes[volume], 0, sizeof volumes[volume]);
    }
}
void filesystem_shutdown(void) {
    for (unsigned i = 0; i < sizeof roots / sizeof *roots; i++) unmount_volume(i);
}
void filesystem_unmount(unsigned volume) {
    if (volume >= sizeof roots / sizeof *roots || !volumes[volume].mounted || !volumes[volume].refs) return;
    if (!--volumes[volume].refs && !volumes[volume].files) unmount_volume(volume);
}

uint32_t filesystem_mount(unsigned volume) {
    if (volume >= sizeof roots / sizeof *roots) return FAILED;
    if (volumes[volume].mounted) { volumes[volume].refs++; return OK; }
    char lock_path[64], path[64], snapshot[64];
    snprintf(lock_path, sizeof lock_path, "%s.lock", roots[volume]);
    snprintf(path, sizeof path, "%s%s", roots[volume], volume == 1 ? "" : ".work");
    snprintf(snapshot, sizeof snapshot, "%s.new", roots[volume]);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return error();
    if (flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return FAILED; }
    if (remove_tree(AT_FDCWD, path)) { close(lock); return FAILED; }
    if (volume == 1) {
        if (mkdir(path, 0700)) { close(lock); return error(); }
    } else {
        struct stat st;
        if (!volume && mkdir(roots[volume], 0700) && errno != EEXIST) { close(lock); return error(); }
        if (lstat(roots[volume], &st)) { uint32_t status = error(); close(lock); return status; }
        if (!S_ISDIR(st.st_mode) || remove_tree(AT_FDCWD, snapshot) || clonefile(roots[volume], path, CLONE_NOFOLLOW)) { close(lock); return FAILED; }
    }
    int root = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) { close(lock); return error(); }
    volumes[volume].root = root; volumes[volume].lock = lock; volumes[volume].mounted = true; volumes[volume].refs = 1;
    if (atexit(filesystem_shutdown)) { filesystem_shutdown(); return FAILED; }
    fprintf(stderr, "FILESYSTEM mounted volume=%u\n", volume);
    return OK;
}

uint32_t filesystem_cache_info(unsigned index, uint64_t *size) {
    if (index >= FILESYSTEM_CACHE_COUNT || !size) return FAILED;
    int fd = open(roots[FILESYSTEM_CACHE_BASE + index], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return error();
    uint64_t limits[2];
    bool valid = fgetxattr(fd, "zelda.cache_limits", limits, sizeof limits, 0, 0) == sizeof limits && limits[0] <= INT64_MAX;
    close(fd);
    if (!valid) return FAILED;
    *size = limits[0]; return OK;
}

uint32_t filesystem_cache_create(unsigned index, uint64_t size, uint64_t journal) {
    if (index >= FILESYSTEM_CACHE_COUNT || size > INT64_MAX || journal > INT64_MAX - size) return FAILED;
    unsigned volume = FILESYSTEM_CACHE_BASE + index;
    if (volumes[volume].mounted) return EXISTS;
    char lock_path[64], path[64];
    snprintf(lock_path, sizeof lock_path, "%s.lock", roots[volume]);
    snprintf(path, sizeof path, "%s.new", roots[volume]);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return error();
    if (flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return FAILED; }
    struct stat st; uint32_t status = OK;
    if (!lstat(roots[volume], &st)) { close(lock); return S_ISDIR(st.st_mode) ? EXISTS : FAILED; }
    if (errno != ENOENT || remove_tree(AT_FDCWD, path) || mkdir(path, 0700)) { close(lock); return FAILED; }
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    const uint64_t limits[] = {size, journal};
    if (fd < 0 || fsetxattr(fd, "zelda.cache_limits", limits, sizeof limits, 0, 0)) status = FAILED;
    if (fd >= 0) close(fd);
    if (!status && renamex_np(path, roots[volume], RENAME_EXCL)) status = error();
    if (status) remove_tree(AT_FDCWD, path);
    close(lock);
    // shortcut: sparse host files use actual disk space; enforce Switch quotas if a title relies on exhaustion.
    if (!status) fprintf(stderr, "FILESYSTEM cache created index=%u size=%llu journal=%llu\n", index, size, journal);
    return status;
}

uint32_t filesystem_cache_delete(unsigned index) {
    if (index >= FILESYSTEM_CACHE_COUNT || volumes[FILESYSTEM_CACHE_BASE + index].mounted) return FAILED;
    unsigned volume = FILESYSTEM_CACHE_BASE + index;
    char lock_path[64]; snprintf(lock_path, sizeof lock_path, "%s.lock", roots[volume]);
    int lock = open(lock_path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0) return error();
    if (flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return FAILED; }
    uint64_t size; uint32_t status = filesystem_cache_info(index, &size);
    if (!status && remove_tree(AT_FDCWD, roots[volume])) status = error();
    close(lock); return status;
}

static int parent_directory(unsigned volume, const char *path, char name[256]) {
    if (volume >= sizeof roots / sizeof *roots || !volumes[volume].mounted || !path || path[0] != '/' || strlen(path) > 0x300) { errno = EINVAL; return -1; }
    char copy[0x301]; strcpy(copy, path);
    int fd = dup(volumes[volume].root);
    if (fd < 0) return -1;
    char *rest = copy, *part;
    strcpy(name, ".");
    while ((part = strsep(&rest, "/"))) {
        if (!*part || !strcmp(part, ".")) continue;
        if (!strcmp(part, "..") || strlen(part) > 255) { close(fd); errno = EINVAL; return -1; }
        if (strcmp(name, ".")) {
            if (!exact_name(fd, name)) { close(fd); return -1; }
            int next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            close(fd); if (next < 0) return -1; fd = next;
        }
        strcpy(name, part);
    }
    return fd;
}

uint32_t filesystem_path(unsigned volume, unsigned command, const char *path, const char *destination,
                         uint64_t argument, local_file **out, uint64_t *value) {
    char name[256]; int parent = parent_directory(volume, path, name);
    if (parent < 0) return error();
    uint32_t result = OK; struct stat st;
    if (command != 7 && command != 8 && !strcmp(name, ".")) { close(parent); return FAILED; }
    if (!fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) { close(parent); return FAILED; }
        // shortcut: case-only collisions are refused; add the reference's xattr aliases if a title needs both names.
        if (!exact_name(parent, name)) { close(parent); return command == 0 || command == 2 ? EXISTS : NOT_FOUND; }
    }
    switch (command) {
        case 0: {
            if (argument > INT64_MAX) { result = FAILED; break; }
            int fd = openat(parent, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd < 0) result = error();
            else {
                if (ftruncate(fd, (off_t)argument)) { result = error(); volumes[volume].poisoned = true; unlinkat(parent, name, 0); }
                close(fd);
            }
            break;
        }
        case 1: if (unlinkat(parent, name, 0)) result = error(); break;
        case 2: if (mkdirat(parent, name, 0700)) result = error(); break;
        case 3: if (unlinkat(parent, name, AT_REMOVEDIR)) result = error(); break;
        case 4: if (remove_tree(parent, name)) result = error(); break;
        case 5: case 6: {
            char target[256]; int to = parent_directory(volume, destination, target);
            if (to < 0) result = error();
            else {
                if (!strcmp(target, ".")) result = FAILED;
                else if (renameatx_np(parent, name, to, target, RENAME_EXCL)) result = error();
                close(to);
            }
            break;
        }
        case 7:
            if (!value) result = FAILED;
            else if (fstatat(parent, name, &st, AT_SYMLINK_NOFOLLOW)) result = error();
            else *value = S_ISDIR(st.st_mode) ? 0 : 1;
            break;
        case 8: {
            if (!out || !(argument & 3) || argument & ~7ull) { result = FAILED; break; }
            int flags = (argument & 2) ? ((argument & 1) ? O_RDWR : O_WRONLY) : O_RDONLY;
            int fd = openat(parent, name, flags | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0) { result = error(); break; }
            if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); result = NOT_FOUND; break; }
            local_file *file = malloc(sizeof *file);
            if (!file) { close(fd); result = FAILED; break; }
            *file = (local_file){fd, volume, (unsigned)argument}; *out = file;
            volumes[volume].files++;
            if (argument & 2) volumes[volume].writers++;
            break;
        }
        default: result = FAILED;
    }
    close(parent); return result;
}

void filesystem_close(local_file *file) {
    if (!file) return;
    unsigned volume = file->volume;
    if (file->mode & 2) volumes[file->volume].writers--;
    if (close(file->fd) && file->mode & 2) volumes[file->volume].poisoned = true;
    free(file);
    if (!--volumes[volume].files && !volumes[volume].refs) unmount_volume(volume);
}

uint32_t filesystem_file(local_file *file, unsigned command, uint64_t offset, void *data, size_t size, uint64_t *value) {
    if (!file || offset > INT64_MAX || size > INT64_MAX - offset) return FAILED;
    struct stat st;
    if (command == 4) {
        if (!value) return FAILED;
        if (fstat(file->fd, &st)) return error();
        *value = st.st_size; return OK;
    }
    if (command == 2) {
        if (!fsync(file->fd)) return OK;
        if (file->mode & 2) volumes[file->volume].poisoned = true;
        return error();
    }
    if (command == 3) {
        if (!(file->mode & 2)) return FAILED;
        if (!ftruncate(file->fd, (off_t)offset)) return OK;
        volumes[file->volume].poisoned = true; return error();
    }
    if (command > 1 || !(file->mode & (command ? 2 : 1)) || (size && !data) || (!command && !value)) return FAILED;
    if (command && !(file->mode & 4)) {
        if (fstat(file->fd, &st)) return error();
        if (offset + size > (uint64_t)st.st_size) { volumes[file->volume].poisoned = true; return FAILED; }
    }
    size_t done = 0;
    while (done < size) {
        ssize_t count = command ? pwrite(file->fd, (uint8_t *)data + done, size - done, (off_t)(offset + done)) :
                                  pread(file->fd, (uint8_t *)data + done, size - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 || (!count && command)) {
            if (command) volumes[file->volume].poisoned = true;
            return count < 0 ? error() : FAILED;
        }
        if (!count) break;
        done += (size_t)count;
    }
    if (value) *value = done;
    return OK;
}

uint32_t filesystem_commit(unsigned volume) {
    if (volume >= sizeof roots / sizeof *roots || !volumes[volume].mounted || volumes[volume].writers || volumes[volume].poisoned) return FAILED;
    if (volume == 1) return OK;
    char path[64], snapshot[64];
    snprintf(path, sizeof path, "%s.work", roots[volume]); snprintf(snapshot, sizeof snapshot, "%s.new", roots[volume]);
    // Same APFS clone-and-swap transaction used by d2r-mobile; only a complete snapshot is published.
    if (remove_tree(AT_FDCWD, snapshot) || clonefile(path, snapshot, CLONE_NOFOLLOW)) return FAILED;
    if (renamex_np(snapshot, roots[volume], RENAME_SWAP)) { remove_tree(AT_FDCWD, snapshot); return FAILED; }
    remove_tree(AT_FDCWD, snapshot);
    fprintf(stderr, "FILESYSTEM transaction committed volume=%u\n", volume);
    return OK;
}
