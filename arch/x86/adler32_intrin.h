/* adler32_intrin.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifndef __ADLER32_INTRIN__
#define __ADLER32_INTRIN__

#ifdef _MSC_VER
#  include <intrin.h>
#else
#  include <x86intrin.h>
#endif

/* These functions mimic intrinsics not available on earlier processors and extend them */

#ifndef _MSC_VER
typedef uint8_t   v1si __attribute__ ((vector_size (16)));
typedef uint16_t  v2si __attribute__ ((vector_size (16)));
typedef uint32_t  v4si __attribute__ ((vector_size (16)));
typedef uint64_t  v8si __attribute__ ((vector_size (16)));
#endif

static inline int64_t haddq_epu64(__m128i a)
{
#ifdef _MSC_VER
  return a.m128i_u64[0] + a.m128i_u64[1];
#else
  v8si b = (v8si)a;
  return b[0] + b[1];
#endif
}

#endif
