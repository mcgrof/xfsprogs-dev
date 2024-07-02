// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_ARCH_H__
#define __XFS_ARCH_H__

#include <linux/swab.h>

#if __BYTE_ORDER == __BIG_ENDIAN
#define	XFS_NATIVE_HOST	1
#else
#undef XFS_NATIVE_HOST
#endif

#ifdef __CHECKER__
# ifndef __bitwise
#  define __bitwise		__attribute__((bitwise))
# endif
#define __force			__attribute__((force))
#else
# ifndef __bitwise
#  define __bitwise
# endif
#define __force
#endif

typedef __u16	__bitwise	__le16;
typedef __u32	__bitwise	__le32;
typedef __u64	__bitwise	__le64;

typedef __u16	__bitwise	__be16;
typedef __u32	__bitwise	__be32;
typedef __u64	__bitwise	__be64;

/*
 * provide defaults when no architecture-specific optimization is detected
 */
#ifndef __arch__swab16
#  define __arch__swab16(x) ({ __u16 __tmp = (x) ; ___swab16(__tmp); })
#endif
#ifndef __arch__swab32
#  define __arch__swab32(x) ({ __u32 __tmp = (x) ; ___swab32(__tmp); })
#endif
#ifndef __arch__swab64
#  define __arch__swab64(x) ({ __u64 __tmp = (x) ; ___swab64(__tmp); })
#endif

#ifndef __arch__swab16p
#  define __arch__swab16p(x) __arch__swab16(*(x))
#endif
#ifndef __arch__swab32p
#  define __arch__swab32p(x) __arch__swab32(*(x))
#endif
#ifndef __arch__swab64p
#  define __arch__swab64p(x) __arch__swab64(*(x))
#endif

#ifndef __arch__swab16s
#  define __arch__swab16s(x) do { *(x) = __arch__swab16p((x)); } while (0)
#endif
#ifndef __arch__swab32s
#  define __arch__swab32s(x) do { *(x) = __arch__swab32p((x)); } while (0)
#endif
#ifndef __arch__swab64s
#  define __arch__swab64s(x) do { *(x) = __arch__swab64p((x)); } while (0)
#endif


#ifdef XFS_NATIVE_HOST
#define cpu_to_be16(val)	((__force __be16)(__u16)(val))
#define cpu_to_be32(val)	((__force __be32)(__u32)(val))
#define cpu_to_be64(val)	((__force __be64)(__u64)(val))
#define be16_to_cpu(val)	((__force __u16)(__be16)(val))
#define be32_to_cpu(val)	((__force __u32)(__be32)(val))
#define be64_to_cpu(val)	((__force __u64)(__be64)(val))

#define cpu_to_le32(val)	((__force __be32)__swab32((__u32)(val)))
#define le32_to_cpu(val)	(__swab32((__force __u32)(__le32)(val)))

#define __constant_cpu_to_le32(val)	\
	((__force __le32)___constant_swab32((__u32)(val)))
#define __constant_cpu_to_be32(val)	\
	((__force __be32)(__u32)(val))
#else
#define cpu_to_be16(val)	((__force __be16)__swab16((__u16)(val)))
#define cpu_to_be32(val)	((__force __be32)__swab32((__u32)(val)))
#define cpu_to_be64(val)	((__force __be64)__swab64((__u64)(val)))
#define be16_to_cpu(val)	(__swab16((__force __u16)(__be16)(val)))
#define be32_to_cpu(val)	(__swab32((__force __u32)(__be32)(val)))
#define be64_to_cpu(val)	(__swab64((__force __u64)(__be64)(val)))

#define cpu_to_le32(val)	((__force __le32)(__u32)(val))
#define le32_to_cpu(val)	((__force __u32)(__le32)(val))

#define __constant_cpu_to_le32(val)	\
	((__force __le32)(__u32)(val))
#define __constant_cpu_to_be32(val)	\
	((__force __be32)___constant_swab32((__u32)(val)))
#endif

static inline void be16_add_cpu(__be16 *a, __s16 b)
{
	*a = cpu_to_be16(be16_to_cpu(*a) + b);
}

static inline void be32_add_cpu(__be32 *a, __s32 b)
{
	*a = cpu_to_be32(be32_to_cpu(*a) + b);
}

static inline void be64_add_cpu(__be64 *a, __s64 b)
{
	*a = cpu_to_be64(be64_to_cpu(*a) + b);
}

static inline uint16_t get_unaligned_be16(const void *p)
{
	const uint8_t *__p = p;
	return __p[0] << 8 | __p[1];
}

static inline uint32_t get_unaligned_be32(const void *p)
{
	const uint8_t *__p = p;
        return (uint32_t)__p[0] << 24 | __p[1] << 16 | __p[2] << 8 | __p[3];
}

static inline uint64_t get_unaligned_be64(const void *p)
{
	return (uint64_t)get_unaligned_be32(p) << 32 |
			   get_unaligned_be32(p + 4);
}

static inline void put_unaligned_be16(uint16_t val, void *p)
{
	uint8_t *__p = p;
	*__p++ = val >> 8;
	*__p++ = val;
}

static inline void put_unaligned_be32(uint32_t val, void *p)
{
	uint8_t *__p = p;
	put_unaligned_be16(val >> 16, __p);
	put_unaligned_be16(val, __p + 2);
}

static inline void put_unaligned_be64(uint64_t val, void *p)
{
	put_unaligned_be32(val >> 32, p);
	put_unaligned_be32(val, p + 4);
}

#endif	/* __XFS_ARCH_H__ */
