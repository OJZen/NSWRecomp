#pragma once
#include <TargetConditionals.h>
#include <mach/mach.h>
#if TARGET_OS_IPHONE
#include <mach/vm_map.h>
// iOS exposes the pointer-width vm API, not the macOS mach_vm header.
_Static_assert(sizeof(vm_address_t) == 8 && sizeof(vm_size_t) == 8, "64-bit guest memory API required");
#define mach_vm_address_t vm_address_t
#define mach_vm_size_t vm_size_t
#define mach_vm_read_overwrite vm_read_overwrite
#define mach_vm_write vm_write
#define mach_vm_remap vm_remap
#else
#include <mach/mach_vm.h>
#endif
