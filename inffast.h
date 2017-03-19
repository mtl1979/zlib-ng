#ifndef INFFAST_H_
#define INFFAST_H_
/* inffast.h -- header to use inffast.c
 * Copyright (C) 1995-2003, 2010 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* WARNING: this file should *not* be used by applications. It is
   part of the implementation of the compression library and is
   subject to change. Applications should only use zlib.h.
 */

void ZLIB_INTERNAL inflate_fast(z_stream *strm, unsigned long start);


#if (defined(__GNUC__) || defined(__clang__)) && defined(__ARM_NEON__)
#  include <arm_neon.h>
typedef uint8x16_t inffast_chunk_t;
#elif defined(HAVE_SSE2_INTRIN) && defined(X86_64)
#  ifdef _MSC_VER
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
typedef __m128i inffast_chunk_t;
#endif

#ifdef inffast_chunk_t
#  define INFFAST_CHUNKSIZE sizeof(inffast_chunk_t)
#endif
#endif /* INFFAST_H_ */
