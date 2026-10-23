#pragma once
#include "vulkan_host.h"

typedef struct { void *module; PFN_vkGetInstanceProcAddr get_instance_proc; } VulkanLoader;
bool vulkan_windows_load(VulkanLoader *loader, char error[256]);
void vulkan_windows_unload(VulkanLoader *loader);
