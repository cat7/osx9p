#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t osx9p_start(kmod_info_t *, void *);
extern kern_return_t osx9p_stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL(org.cat7.osx9p, "1.1.1", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = osx9p_start;
__private_extern__ kmod_stop_func_t *_antimain = osx9p_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
