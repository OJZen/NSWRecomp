#include "vulkan_windows.h"
#include <windows.h>
#include <stdio.h>

bool vulkan_windows_load(VulkanLoader *loader, char error[256]) {
    *loader = (VulkanLoader){0};
    HMODULE module = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { snprintf(error, 256, "system Vulkan loader unavailable: Windows error %lu", GetLastError()); return false; }
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(module, "vkGetInstanceProcAddr");
    if (!get) { FreeLibrary(module); snprintf(error, 256, "system Vulkan loader has no vkGetInstanceProcAddr"); return false; }
    *loader = (VulkanLoader){module, get};
    return true;
}

void vulkan_windows_unload(VulkanLoader *loader) {
    if (loader->module) FreeLibrary(loader->module);
    *loader = (VulkanLoader){0};
}
