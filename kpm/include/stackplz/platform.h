#ifndef STACKPLZ_PLATFORM_H
#define STACKPLZ_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#if defined(SPZ_KPATCH_BUILD)
#include <linux/string.h>
#include <uapi/asm-generic/errno.h>
#ifdef barrier
#undef barrier
#endif
#else
#include <errno.h>
#include <string.h>
#endif

#endif
