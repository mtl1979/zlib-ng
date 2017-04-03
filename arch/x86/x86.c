/*
 * x86 feature check
 *
 * Copyright (C) 2013 Intel Corporation. All rights reserved.
 * Author:
 *  Jim Kukunas
 *
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#include "x86.h"

#ifdef _MSC_VER
#  include <intrin.h>
#else
/* Newer versions of GCC and clang come with cpuid.h */
#  include <cpuid.h>
#  include <stdint.h>
#if (__GNUC__ > 4 || __GNUC__ == 4 && __GNUC_MINOR__ >= 4) ||  defined(__clang__)
  static inline uint64_t _xgetbv(unsigned int index) {
    unsigned int eax, edx;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(index));
    return ((uint64_t)edx << 32) | eax;
  }
#else
#define _xgetbv() 0
#endif
#endif

ZLIB_INTERNAL int x86_cpu_has_sse2;
ZLIB_INTERNAL int x86_cpu_has_sse3;
ZLIB_INTERNAL int x86_cpu_has_ssse3;
ZLIB_INTERNAL int x86_cpu_has_sse41;
ZLIB_INTERNAL int x86_cpu_has_sse42;
ZLIB_INTERNAL int x86_cpu_has_pclmulqdq;
ZLIB_INTERNAL int x86_cpu_has_tzcnt;
ZLIB_INTERNAL int x86_cpu_has_xop;
ZLIB_INTERNAL int x86_cpu_has_avx;
ZLIB_INTERNAL int x86_cpu_has_avx2;

static void cpuid(int info, unsigned* eax, unsigned* ebx, unsigned* ecx, unsigned* edx) {
#ifdef _MSC_VER
	unsigned int registers[4];
	__cpuid(registers, info);

	*eax = registers[0];
	*ebx = registers[1];
	*ecx = registers[2];
	*edx = registers[3];
#else
	unsigned int _eax;
	unsigned int _ebx;
	unsigned int _ecx;
	unsigned int _edx;
	__cpuid(info, _eax, _ebx, _ecx, _edx);
	*eax = _eax;
	*ebx = _ebx;
	*ecx = _ecx;
	*edx = _edx;
#endif
}

static int avx_enabled() {
	return _xgetbv(0) & 6;
}

void ZLIB_INTERNAL x86_check_features(void) {
	unsigned eax, ebx, ecx, edx;
	cpuid(1 /*CPU_PROCINFO_AND_FEATUREBITS*/, &eax, &ebx, &ecx, &edx);

	x86_cpu_has_sse2 = edx & 0x4000000;
	x86_cpu_has_sse3 = ecx & 0x1;
	x86_cpu_has_ssse3 = ecx & 0x200;
	x86_cpu_has_sse41 = ecx & 0x80000;
	x86_cpu_has_sse42 = ecx & 0x100000;
	x86_cpu_has_pclmulqdq = ecx & 0x2;
	x86_cpu_has_avx = (ecx & 0x18000000) && avx_enabled();

	cpuid(7, &eax, &ebx, &ecx, &edx);

	x86_cpu_has_avx2 = ebx & 0x20;
	x86_cpu_has_tzcnt = ecx & 0x8;

	cpuid(0x80000001, &eax, &ebx, &ecx, &edx);

	x86_cpu_has_xop = ecx & 0x4000;
}
