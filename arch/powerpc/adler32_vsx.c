/* adler32_vsx.c -- compute the Adler-32 checksum of a data stream
 * Copyright (C) 1995-2011 Mark Adler
 * Copyright (C) 2017 Mika T. Lindqvist
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* @(#) $Id$ */

#ifdef HAVE_PPC_ALTIVEC

#include <altivec.h>
#include "adler32_ppc.h"

#define vmx_zero()  (vec_splat_u32(0))

static inline void vsx_handle_tail(uint32_t *pair, const unsigned char *buf, size_t len)
{
  unsigned int i;
  for (i = 0; i < len; ++i) {
    pair[0] += buf[i];
    pair[1] += pair[0];
  }
}

static void vsx_accum32(uint32_t *s, const unsigned char *buf, size_t len)
{
  static const uint8_t tc0[16] = {16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  3,  2,  1};

  vector unsigned char t0 = vec_vsx_ld(0, tc0);
  vector unsigned int  adacc, s2acc;
  adacc = vec_insert(s[0], vmx_zero(), 0);
  s2acc = vec_insert(s[1], vmx_zero(), 0);

  while (len > 0) {
    vector unsigned char d0 = vec_vsx_ld(0, buf);
    vector unsigned short sum2;
    sum2  = vec_add(vec_mulo(t0, d0), vec_mule(t0, d0));
    s2acc = vec_add(s2acc, vec_sl(adacc, vec_splat_u32(4)));
    s2acc = vec_add(s2acc, vec_hadduw(sum2));
    adacc = vec_add(adacc, vec_hadduw(vec_hadduh(d0)));
    buf += 16;
    len--;
  }

  s[0] = vec_extract(adacc, 0) + vec_extract(adacc, 1) + vec_extract(adacc, 2) + vec_extract(adacc, 3); /* Horizontal add */
  s[1] = vec_extract(s2acc, 0) + vec_extract(s2acc, 1) + vec_extract(s2acc, 2) + vec_extract(s2acc, 3); /* Horizontal add */
}

uint32_t adler32_vsx(uint32_t adler, const unsigned char *buf, size_t len)
{
  /* The largest prime smaller than 65536. */
  const uint32_t M_BASE = 65521;
  /* This is the threshold where doing accumulation may overflow. */
  const int M_NMAX = 5552;

  uint32_t sum2;
  uint32_t pair[2];
  int n = M_NMAX;
  unsigned int done = 0, i;

  if (buf == NULL)
    return 1L;

  /* Split Adler-32 into component sums, it can be supplied by
   * the caller sites (e.g. in a PNG file).
   */
  sum2 = (adler >> 16) & 0xffff;
  adler &= 0xffff;
  pair[0] = adler;
  pair[1] = sum2;

  for (i = 0; i < len; i += n) {
    if ((i + n) > len)
      n = (int)(len - i);

    if (n < 16)
      break;

    vsx_accum32(pair, buf + i, n / 16);
    pair[0] %= M_BASE;
    pair[1] %= M_BASE;

    done += (n / 16) * 16;
  }

  /* Handle the tail elements. */
  if (done < len) {
    vsx_handle_tail(pair, (buf + done), len - done);
    pair[0] %= M_BASE;
    pair[1] %= M_BASE;
  }

  /* D = B * 65536 + A, see: https://en.wikipedia.org/wiki/Adler-32. */
  return (pair[1] << 16) | pair[0];
}
#endif
