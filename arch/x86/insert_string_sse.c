/* insert_string_sse -- insert_string variant using SSE4.2's CRC instructions
 *
 * Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 */

#include "deflate.h"

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * IN  assertion: all calls to to INSERT_STRING are made with consecutive
 *    input characters and the first MIN_MATCH bytes of str are valid
 *    (except for the last MIN_MATCH-1 bytes of the input file).
 */
#ifdef X86_SSE4_2_CRC_HASH
Pos insert_string_sse(deflate_state *const s, const Pos str, unsigned int count) {
    Pos p, lp, ret;

    if (unlikely(count == 0)) {
        return s->prev[str & s->w_mask];
    }

    ret = 0;
    lp = str + count - 1; /* last position */

    for (p = str; p <= lp; p++) {
        unsigned *ip, val, h, hm;

        ip = (unsigned *)&s->window[p];
        val = *ip;

        if (s->level >= TRIGGER_LEVEL)
            val &= 0xFFFFFF;

#ifdef _MSC_VER
        h = _mm_crc32_u32(0, val);
#else
        h = 0;

        __asm__ __volatile__ (
            "crc32 %1,%0\n\t"
            : "+r" (h)
            : "r" (val)
        );
#endif

        hm = h & s->hash_mask; /* masked hash value */

        UPDATE_PREV(s, hm, p);
    }
    return ret;
}
#endif
