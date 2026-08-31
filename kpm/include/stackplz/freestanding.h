#ifndef STACKPLZ_FREESTANDING_H
#define STACKPLZ_FREESTANDING_H

/* KPatch's intentionally small stdint.h provides types but not C99 macros. */
#ifndef UINT8_C
#define UINT8_C(value) value##U
#endif
#ifndef UINT16_C
#define UINT16_C(value) value##U
#endif
#ifndef UINT32_C
#define UINT32_C(value) value##U
#endif
#ifndef UINT64_C
#define UINT64_C(value) value##UL
#endif

#ifndef UINT8_MAX
#define UINT8_MAX UINT8_C(0xff)
#endif
#ifndef UINT16_MAX
#define UINT16_MAX UINT16_C(0xffff)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX UINT32_C(0xffffffff)
#endif
#ifndef UINT64_MAX
#define UINT64_MAX UINT64_C(0xffffffffffffffff)
#endif

#endif
