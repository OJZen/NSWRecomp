# Completion callback mutex guards

An adapter can explicitly bind a compiled completion callback to the same guest
mutex used by its waiter. This addresses a specific notify-before-wait ordering
defect when the completion callback omits that mutex. It does not modify kernel
condition variables, retain early notifications or manufacture successful waits.
Use it only after inspecting the exact callback, waiter and mutex ownership.

Windows basic-services startup accepts `-CallbackMutexGuards ./local/guards.json`
(Python: `--callback-mutex-guards ./local/guards.json`). The option is off by
default and supports at most 16 bindings in a 64 KiB JSON file. Keep title-specific
configuration, code fingerprints and evidence under ignored `local/` or `work/`.
Its schema is:

```json
{
  "schema": 1,
  "guards": [{
    "module": "main",
    "module_sha256": "YOUR_VERIFIED_MODULE_SHA256",
    "function_offset": "0x80",
    "function_size": 4,
    "function_sha256": "YOUR_VERIFIED_FUNCTION_BYTES_SHA256",
    "context_pointer_offset": 8,
    "mutex_offset": 16,
    "mutex_size": 64,
    "lock_symbol": "YOUR_COMPILED_MUTEX_LOCK_EXPORT",
    "unlock_symbol": "YOUR_COMPILED_MUTEX_UNLOCK_EXPORT"
  }]
}
```

The example offsets describe a synthetic layout, not a title adapter. The
callback must receive its closure in x0. `context_pointer_offset` locates an
object pointer inside that closure; `mutex_offset` locates the mutex in that
object. `mutex_size` is its required writable extent. Each helper takes the mutex
pointer as its only argument and returns normally on success.

`tools/callback_guards.py` checks the complete module fingerprint, the selected
function byte span, object bounds and two distinct executable helper exports.
It emits a small C binding and LLVM MinGW linker wrapping flags. This covers
dispatch-table entries and references from other translation units. Direct calls
resolved within the target's own translation unit can bypass wrapping; the
adapter must establish that the selected entry is an indirect completion callback.
This mechanism is not a general function replacement API.

`runtime/callback_guard.c` checks the closure/object memory, acquires the compiled
guest mutex, calls the original compiled callback and releases the mutex after
normal return. Helper re-entry preserves callback arguments/results, guest TLS
and stack through the existing guest-callback contract. Nonlocal exception or
abort recovery is outside this contract. Invalid runtime bindings stop with
`adapter_guard_error` instead of continuing with an unprotected callback.

Receipts include the configuration and generated-binding SHA-256, resolved
addresses and input fingerprints. The native worker embeds the configuration
hash and counts completed guarded calls. The supervisor rejects a different
embedded hash. Enabling a binding does not regenerate or modify guest C objects.

Synthetic tests force the completion thread to reach the guard before the worker
can publish its wait. The unguarded control fails; both wrapped dispatch-table
and cross-unit direct calls complete with register/TLS preservation. Separate
checks reject altered code, missing helpers and invalid object layouts. Actual
title stability still requires bounded repeat runs with matching inputs.
