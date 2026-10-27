/*
 * Classic kmod descriptor for command-line builds (see kext/Makefile).
 * Xcode generates this itself; with a plain clang link it has to exist or
 * kmutil refuses the kext with "kexts must have a _kmod_info symbol".
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.illia.RyzenPlatform, "0.1.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
