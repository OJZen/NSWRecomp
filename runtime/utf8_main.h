// Windows command lines are UTF-16; the runtime path interface is UTF-8.
// Include before a definition of main, and link with -municode on Windows.
#pragma once
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
static int utf8_main(int argc, char **argv);
int wmain(int argc, wchar_t **wide) {
    char **args = calloc((size_t)argc + 1, sizeof *args);
    if (!args) return 1;
    int result = 1;
    for (int i = 0; i < argc; i++) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!size || !(args[i] = malloc((size_t)size)) ||
                !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, args[i], size, NULL, NULL))
            goto done;
    }
    result = utf8_main(argc, args);
done:
    for (int i = 0; i < argc; i++) free(args[i]);
    free(args);
    return result;
}
#define main utf8_main
#endif
