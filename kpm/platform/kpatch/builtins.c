#include <stddef.h>
#include <stdint.h>

#if defined(SPZ_KPATCH_BUILD)

/*
 * KPatch exports kf_* as function pointers (kfunc_def), not direct functions.
 * A `b kf_memcpy` tail-call would jump into the pointer word and reboot.
 */
extern void *(*kf_memcpy)(void *destination, const void *source, size_t length);
extern void *(*kf_memset)(void *destination, int value, size_t length);

/* GCC lowers some fixed-size aggregate assignments to these ABI symbols. */
void *memcpy(void *destination, const void *source, size_t length)
{
    return kf_memcpy(destination, source, length);
}

void *memset(void *destination, int value, size_t length)
{
    return kf_memset(destination, value, length);
}

#endif
