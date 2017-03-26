/* adler32_ppc.h -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifndef _ADLER32_PPC_H_
#define _ADLER32_PPC_H_

#include <altivec.h>
#include "zlib.h"
#include "zutil.h"


static inline vector unsigned short vec_hadduh(vector unsigned char a)
{
  vector unsigned char vmx_one = vec_splat_u8(1);
  return vec_add(vec_mulo(a, vmx_one), vec_mule(a, vmx_one));
}

static inline vector unsigned int vec_hadduw(vector unsigned short a)
{
  vector unsigned short vmx_one = vec_splat_u16(1);
  return vec_add(vec_mulo(a, vmx_one), vec_mule(a, vmx_one));
}

#endif
