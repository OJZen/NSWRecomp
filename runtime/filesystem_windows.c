// Windows scratch filesystems for bounded initialization. No persistent saves.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "filesystem.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { OK = 0, NOT_FOUND = 0x202, EXISTS = 0x402, FAILED = (5305 << 9) | 2,
       VOLUMES = FILESYSTEM_CACHE_BASE + FILESYSTEM_CACHE_COUNT, PATH_CAP = 4096 };
#define SCRATCH_LIMIT (UINT64_C(128) << 20)
static HANDLE root;
static wchar_t root_path[PATH_CAP];
static uint64_t used;
static struct { unsigned refs, files, writers; bool created, poisoned; uint64_t quota; } volumes[VOLUMES];
struct local_file { HANDLE handle; unsigned volume, mode; };

static uint32_t error(void) {
    DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? NOT_FOUND :
           code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ? EXISTS : FAILED;
}
static bool directory(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}
static bool initialize(void) {
    if (root) return true;
    wchar_t path[PATH_CAP];
    DWORD length = GetEnvironmentVariableW(L"NSW_SCRATCH_FS", path, PATH_CAP);
    if (!length || length >= PATH_CAP || !directory(path)) return false;
    HANDLE handle = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (handle == INVALID_HANDLE_VALUE) return false;
    length = GetFinalPathNameByHandleW(handle, root_path, PATH_CAP, FILE_NAME_NORMALIZED);
    if (!length || length >= PATH_CAP - 0x400) { CloseHandle(handle); return false; }
    root = handle;
    return true;
}
static void volume_path(unsigned volume, wchar_t output[PATH_CAP]) {
    swprintf(output, PATH_CAP, L"%ls\\volume-%u", root_path, volume);
}

// Reject aliases, alternate streams, parent traversal, device names, symlinks
// and junctions. Existing names must match the guest's case-sensitive spelling.
static uint32_t path_for(unsigned volume, const char *path, wchar_t output[PATH_CAP]) {
    if (volume >= VOLUMES || !volumes[volume].refs || !path || path[0] != '/' || strlen(path) > 0x300) return FAILED;
    volume_path(volume, output);
    const char *cursor = path + 1;
    while (*cursor) {
        const char *end = strchr(cursor, '/'); size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (!length || length > 255 || (length == 1 && *cursor == '.') ||
                (length == 2 && !memcmp(cursor, "..", 2)) || cursor[length - 1] == '.' || cursor[length - 1] == ' ') return FAILED;
        for (size_t i = 0; i < length; i++)
            if ((unsigned char)cursor[i] < 32 || strchr("\\:*?\"<>|", cursor[i])) return FAILED;
        wchar_t name[256]; int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cursor, (int)length, name, 255);
        if (!count) return FAILED;
        name[count] = 0;
        wchar_t base[256]; wcscpy(base, name); wchar_t *dot = wcschr(base, '.'); if (dot) *dot = 0;
        if (!_wcsicmp(base, L"CON") || !_wcsicmp(base, L"PRN") || !_wcsicmp(base, L"AUX") || !_wcsicmp(base, L"NUL") ||
            (!_wcsnicmp(base, L"COM", 3) && wcslen(base) == 4 && base[3] >= L'0' && base[3] <= L'9') ||
            (!_wcsnicmp(base, L"LPT", 3) && wcslen(base) == 4 && base[3] >= L'0' && base[3] <= L'9')) return FAILED;
        wcscat(output, L"\\"); wcscat(output, name);
        WIN32_FIND_DATAW info; HANDLE find = FindFirstFileW(output, &info);
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
            if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return FAILED;
            if (wcscmp(info.cFileName, name)) return NOT_FOUND;
            if (end && !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return NOT_FOUND;
        } else if (end) return error();
        if (!end) break;
        cursor = end + 1;
        if (!*cursor) return FAILED;
    }
    return OK;
}

uint32_t filesystem_mount(unsigned volume) {
    if (volume >= VOLUMES || !initialize()) return FAILED;
    wchar_t path[PATH_CAP]; volume_path(volume, path);
    if (!volumes[volume].created) {
        // Never reuse a pre-existing directory: the launcher owns a fresh root.
        if (!CreateDirectoryW(path, NULL)) return error();
        volumes[volume].created = true;
    }
    if (!directory(path)) return FAILED;
    volumes[volume].refs++;
    fprintf(stderr, "FILESYSTEM scratch mounted volume=%u\n", volume);
    return OK;
}
void filesystem_unmount(unsigned volume) {
    if (volume < VOLUMES && volumes[volume].refs) volumes[volume].refs--;
}
void filesystem_shutdown(void) {
    for (unsigned i = 0; i < VOLUMES; i++) if (volumes[i].files) return;
    if (root) CloseHandle(root);
    root = NULL; used = 0; memset(volumes, 0, sizeof volumes);
}
uint32_t filesystem_cache_info(unsigned index, uint64_t *size) {
    if (index >= FILESYSTEM_CACHE_COUNT || !size) return FAILED;
    if (!volumes[index + FILESYSTEM_CACHE_BASE].quota) return NOT_FOUND;
    *size = volumes[index + FILESYSTEM_CACHE_BASE].quota; return OK;
}
uint32_t filesystem_cache_create(unsigned index, uint64_t size, uint64_t journal) {
    if (index >= FILESYSTEM_CACHE_COUNT || !size || size > SCRATCH_LIMIT || journal > SCRATCH_LIMIT - size) return FAILED;
    unsigned volume = index + FILESYSTEM_CACHE_BASE;
    if (volumes[volume].quota) return EXISTS;
    volumes[volume].quota = size; return OK;
}
uint32_t filesystem_cache_delete(unsigned index) { (void)index; return FAILED; }

static bool length_of(HANDLE file, uint64_t *size) {
    LARGE_INTEGER length;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) return false;
    *size = (uint64_t)length.QuadPart; return true;
}
static bool seek(HANDLE file, uint64_t offset) {
    LARGE_INTEGER position; position.QuadPart = (LONGLONG)offset;
    return offset <= INT64_MAX && SetFilePointerEx(file, position, NULL, FILE_BEGIN);
}
static uint32_t resize(local_file *file, uint64_t size) {
    uint64_t previous;
    if (size > SCRATCH_LIMIT || !length_of(file->handle, &previous) || previous > used ||
            (size > previous && size - previous > SCRATCH_LIMIT - used)) return FAILED;
    if (!seek(file->handle, size) || !SetEndOfFile(file->handle)) { volumes[file->volume].poisoned = true; return FAILED; }
    used = used - previous + size; return OK;
}

uint32_t filesystem_path(unsigned volume, unsigned command, const char *path, const char *destination,
                         uint64_t argument, local_file **out, uint64_t *value) {
    wchar_t full[PATH_CAP], to[PATH_CAP]; uint32_t status = path_for(volume, path, full);
    if (status) return status;
    if (!strcmp(path, "/") && command != 7) return FAILED;
    switch (command) {
    case 0: {
        if (argument > SCRATCH_LIMIT - used) return FAILED;
        HANDLE handle = CreateFileW(full, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (handle == INVALID_HANDLE_VALUE) return error();
        local_file file = {handle, volume, 2}; status = resize(&file, argument);
        CloseHandle(handle); if (status) DeleteFileW(full);
        return status;
    }
    case 1: {
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (!GetFileAttributesExW(full, GetFileExInfoStandard, &info)) return error();
        uint64_t size = (uint64_t)info.nFileSizeHigh << 32 | info.nFileSizeLow;
        if (size > used || !DeleteFileW(full)) return error();
        used -= size; return OK;
    }
    case 2: return CreateDirectoryW(full, NULL) ? OK : error();
    case 3: return RemoveDirectoryW(full) ? OK : error();
    case 5: case 6:
        status = path_for(volume, destination, to);
        if (status || !strcmp(destination, "/")) return status ? status : FAILED;
        return MoveFileExW(full, to, 0) ? OK : error();
    case 7: {
        DWORD attr = GetFileAttributesW(full);
        if (!value) return FAILED;
        if (attr == INVALID_FILE_ATTRIBUTES) return error();
        *value = attr & FILE_ATTRIBUTE_DIRECTORY ? 0 : 1; return OK;
    }
    case 8: {
        if (!out || !(argument & 3) || argument & ~UINT64_C(7)) return FAILED;
        DWORD access = (argument & 1 ? GENERIC_READ : 0) | (argument & 2 ? GENERIC_WRITE : 0);
        HANDLE handle = CreateFileW(full, access, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
        if (handle == INVALID_HANDLE_VALUE) return error();
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            CloseHandle(handle); return FAILED;
        }
        local_file *file = malloc(sizeof *file);
        if (!file) { CloseHandle(handle); return FAILED; }
        *file = (local_file){handle, volume, (unsigned)argument}; *out = file;
        volumes[volume].files++; volumes[volume].writers += !!(argument & 2); return OK;
    }
    default: return FAILED;
    }
}
void filesystem_close(local_file *file) {
    if (!file) return;
    if (!CloseHandle(file->handle)) volumes[file->volume].poisoned = true;
    volumes[file->volume].files--; volumes[file->volume].writers -= !!(file->mode & 2);
    free(file);
}
uint32_t filesystem_file(local_file *file, unsigned command, uint64_t offset, void *data, size_t size, uint64_t *value) {
    if (!file || offset > INT64_MAX || size > INT64_MAX - offset) return FAILED;
    if (command == 4) return value && length_of(file->handle, value) ? OK : FAILED;
    if (command == 2) return !(file->mode & 2) || FlushFileBuffers(file->handle) ? OK : FAILED;
    if (command == 3) return file->mode & 2 ? resize(file, offset) : FAILED;
    if (command > 1 || !(file->mode & (command ? 2 : 1)) || (size && !data) || (!command && !value)) return FAILED;
    uint64_t current;
    if (!length_of(file->handle, &current)) return FAILED;
    if (command && offset + size > current) {
        if (!(file->mode & 4)) return FAILED;
        uint32_t status = resize(file, offset + size); if (status) return status;
    }
    if (!seek(file->handle, offset)) return FAILED;
    size_t done = 0;
    while (done < size) {
        DWORD count = (DWORD)(size - done < 65536 ? size - done : 65536), got = 0;
        BOOL ok = command ? WriteFile(file->handle, (char *)data + done, count, &got, NULL) :
                            ReadFile(file->handle, (char *)data + done, count, &got, NULL);
        if (!ok || (!got && command)) { if (command) volumes[file->volume].poisoned = true; return FAILED; }
        if (!got) break;
        done += got;
    }
    if (value) *value = done;
    return OK;
}
uint32_t filesystem_commit(unsigned volume) {
    if (volume >= VOLUMES || !volumes[volume].refs || volumes[volume].writers || volumes[volume].poisoned) return FAILED;
    // Closing all writers establishes visibility within this disposable run.
    // The launcher deletes the entire fresh root after the process exits.
    return OK;
}
#endif
