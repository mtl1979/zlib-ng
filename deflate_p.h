/* deflate_p.h -- Private inline functions and macros shared with more than
 *                one deflate method
 *
 * Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 */

#ifndef DEFLATE_P_H
#define DEFLATE_P_H

#if defined(X86_CPUID)
# include "arch/x86/x86.h"
#endif

/* Forward declare common non-inlined functions declared in deflate.c */

#ifdef ZLIB_DEBUG
void check_match(deflate_state *s, IPos start, IPos match, int length);
#else
#define check_match(s, start, match, length)
#endif
void fill_window(deflate_state *s);
void flush_pending(z_stream *strm);

/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * IN  assertion: all calls to to INSERT_STRING are made with consecutive
 *    input characters and the first MIN_MATCH bytes of str are valid
 *    (except for the last MIN_MATCH-1 bytes of the input file).
 */

#ifdef X86_SSE4_2_CRC_HASH
extern Pos insert_string_sse(deflate_state *const s, const Pos str, unsigned int count);
#elif defined(ARM_ACLE_CRC_HASH)
extern Pos insert_string_acle(deflate_state *const s, const Pos str, unsigned int count);
#endif

static inline Pos insert_string_c(deflate_state *const s, const Pos str, unsigned int count) {
    Pos p, lp;
    unsigned int h = s->ins_h;

    if (unlikely(count == 0)) {
        return s->prev[str & s->w_mask];
    }

    lp = str + count - 1; /* last position */

    for (p = str; p <= lp; p++) {
        UPDATE_HASH(s, h, p);
        UPDATE_PREV(s, h, p);
    }
    s->ins_h = h;
    return s->prev[lp & s->w_mask];
}

static inline Pos insert_string(deflate_state *const s, const Pos str, unsigned int count) {
#ifdef X86_SSE4_2_CRC_HASH
    if (x86_cpu_has_sse42)
        return insert_string_sse(s, str, count);
#endif
#if defined(ARM_ACLE_CRC_HASH)
    return insert_string_acle(s, str, count);
#else
    return insert_string_c(s, str, count);
#endif
}

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, last) { \
    _tr_flush_block(s, (s->block_start >= 0L ? \
                   (char *)&s->window[(unsigned)s->block_start] : \
                   NULL), \
                   (unsigned long)((long)s->strstart - s->block_start), \
                   (last)); \
    s->block_start = s->strstart; \
    flush_pending(s->strm); \
    Tracev((stderr, "[FLUSH]")); \
}

/* Same but force premature exit if necessary. */
#define FLUSH_BLOCK(s, last) { \
    FLUSH_BLOCK_ONLY(s, last); \
    if (s->strm->avail_out == 0) return (last) ? finish_started : need_more; \
}

static inline void fill_internal(deflate_state *s) {
    unsigned int insert_cnt = s->insert;
    unsigned int str = s->strstart - insert_cnt;
    unsigned int slen;
    s->ins_h = s->window[str];
    if (unlikely(s->lookahead < MIN_MATCH)) {
        insert_cnt += s->lookahead - MIN_MATCH;
    }
    slen = insert_cnt;
    if (likely(str >= (MIN_MATCH - 2))) {
        str += 2 - MIN_MATCH;
        insert_cnt += MIN_MATCH - 2;
    }
    if (likely(insert_cnt > 0)) {
        insert_string(s, str, insert_cnt);
        s->insert -= slen;
    }
}

/* Maximum stored block length in deflate format (not including header). */
#define MAX_STORED 65535

/* Minimum of a and b. */
#define MIN(a, b) ((a) > (b) ? (b) : (a))

#endif
