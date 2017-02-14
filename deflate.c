/* deflate.c -- compress data using the deflation algorithm
 * Copyright (C) 1995-2016 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/*
 *  ALGORITHM
 *
 *      The "deflation" process depends on being able to identify portions
 *      of the input text which are identical to earlier input (within a
 *      sliding window trailing behind the input currently being processed).
 *
 *      The most straightforward technique turns out to be the fastest for
 *      most input files: try all possible matches and select the longest.
 *      The key feature of this algorithm is that insertions into the string
 *      dictionary are very simple and thus fast, and deletions are avoided
 *      completely. Insertions are performed at each input character, whereas
 *      string matches are performed only when the previous match ends. So it
 *      is preferable to spend more time in matches to allow very fast string
 *      insertions and avoid deletions. The matching algorithm for small
 *      strings is inspired from that of Rabin & Karp. A brute force approach
 *      is used to find longer strings when a small match has been found.
 *      A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 *      (by Leonid Broukhis).
 *         A previous version of this file used a more sophisticated algorithm
 *      (by Fiala and Greene) which is guaranteed to run in linear amortized
 *      time, but has a larger average cost, uses more memory and is patented.
 *      However the F&G algorithm may be faster for some highly redundant
 *      files if the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 *      The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 *      I found it in 'freeze' written by Leonid Broukhis.
 *      Thanks to many people for bug reports and testing.
 *
 *  REFERENCES
 *
 *      Deutsch, L.P.,"DEFLATE Compressed Data Format Specification".
 *      Available in http://tools.ietf.org/html/rfc1951
 *
 *      A description of the Rabin and Karp algorithm is given in the book
 *         "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 *      Fiala,E.R., and Greene,D.H.
 *         Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 */

/* @(#) $Id$ */

#include "deflate.h"
#include "deflate_p.h"
#include "match.h"

const char deflate_copyright[] = " deflate 1.2.11.f Copyright 1995-2016 Jean-loup Gailly and Mark Adler ";
/*
  If you use the zlib library in a product, an acknowledgment is welcome
  in the documentation of your product. If for some reason you cannot
  include such an acknowledgment, I would appreciate that you keep this
  copyright string in the executable of your product.
 */

/* ===========================================================================
 *  Function prototypes.
 */

typedef block_state (*compress_func) (deflate_state *s, int flush);
/* Compression function. Returns the block state after the call. */

static int deflateStateCheck      (z_stream *strm);
static void slide_hash            (deflate_state *s);
static block_state deflate_stored (deflate_state *s, int flush);
block_state deflate_fast          (deflate_state *s, int flush);
block_state deflate_quick         (deflate_state *s, int flush);
#ifdef MEDIUM_STRATEGY
block_state deflate_medium        (deflate_state *s, int flush);
#endif
block_state deflate_slow          (deflate_state *s, int flush);
static block_state deflate_rle    (deflate_state *s, int flush);
static block_state deflate_huff   (deflate_state *s, int flush);
static void lm_init               (deflate_state *s);
static void putShortMSB           (deflate_state *s, uint16_t b);
ZLIB_INTERNAL void flush_pending  (z_stream *strm);
ZLIB_INTERNAL unsigned read_buf   (z_stream *strm, unsigned char *buf, unsigned size);

extern void crc_reset(deflate_state *const s);
#ifdef X86_PCLMULQDQ_CRC
extern void crc_finalize(deflate_state *const s);
#endif
extern void copy_with_crc(z_stream *strm, unsigned char *dst, unsigned long size);

/* ===========================================================================
 * Local data
 */

#define NIL 0
/* Tail of hash chains */

/* Values for max_lazy_match, good_match and max_chain_length, depending on
 * the desired pack level (0..9). The values given below have been tuned to
 * exclude worst case performance for pathological files. Better values may be
 * found for specific files.
 */
typedef struct config_s {
    uint16_t good_length; /* reduce lazy search above this match length */
    uint16_t max_lazy;    /* do not perform lazy search above this match length */
    uint16_t nice_length; /* quit search above this match length */
    uint16_t max_chain;
    compress_func func;
} config;

static const config configuration_table[10] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, deflate_stored},  /* store only */

#ifdef X86_QUICK_STRATEGY
/* 1 */ {4,    4,  8,    4, deflate_quick},
/* 2 */ {4,    4,  8,    4, deflate_fast}, /* max speed, no lazy matches */
#else
/* 1 */ {4,    4,  8,    4, deflate_fast}, /* max speed, no lazy matches */
/* 2 */ {4,    5, 16,    8, deflate_fast},
#endif

/* 3 */ {4,    6, 32,   32, deflate_fast},

#ifdef MEDIUM_STRATEGY
#  ifdef NOT_TWEAK_COMPILER
/* 4 */ {4,    4, 16,   16, deflate_medium},  /* lazy matches */
/* 5 */ {8,   16, 32,   32, deflate_medium},
/* 6 */ {8,   16, 128, 128, deflate_medium},
#  else
/* 4 */ {4,    8, 16,   16, deflate_medium},  /* lazy matches */
/* 5 */ {8,   16, 64,  128, deflate_medium},
/* 6 */ {8,   16, 128, 128, deflate_slow},
#  endif /* NOT_TWEAK_COMPILER */
#else
/* 4 */ {4,   12, 32,   32, deflate_slow},  /* lazy matches */
/* 5 */ {8,   16, 48,   64, deflate_slow},
/* 6 */ {8,   32, 128, 128, deflate_slow},
#endif

/* 7 */ {8,   32, 128,  256, deflate_slow},
/* 8 */ {32, 128, 258, 1024, deflate_slow},
/* 9 */ {32, 258, 258, 4096, deflate_slow}}; /* max compression */

/* Note: the deflate() code requires max_lazy >= MIN_MATCH and max_chain >= 4
 * For deflate_fast() (levels <= 3) good is ignored and lazy has a different
 * meaning.
 */

/* rank Z_BLOCK between Z_NO_FLUSH and Z_PARTIAL_FLUSH */
#define RANK(f) (((f) * 2) - ((f) > 4 ? 9 : 0))


/* ===========================================================================
 * Initialize the hash table (avoiding 64K overflow for 16 bit systems).
 * prev[] will be initialized on the fly.
 */
#define CLEAR_HASH(s) \
    s->head[s->hash_size-1] = NIL; \
    memset((unsigned char *)s->head, 0, (unsigned)(s->hash_size-1)*sizeof(*s->head));

/* ===========================================================================
 * Slide the hash table when sliding the window down (could be avoided with 32
 * bit values at the expense of memory usage). We slide even when level == 0 to
 * keep the hash table consistent if we switch back to level > 0 later.
 */
static void slide_hash(deflate_state *s) {
    unsigned n, m;
    Pos *p;
    unsigned int wsize = s->w_size;

    n = s->hash_size;
    p = &s->head[n];
    do {
        m = *--p;
        *p = (Pos)(m >= wsize ? m - wsize : NIL);
    } while (--n);
    n = wsize;
#ifndef FASTEST
    p = &s->prev[n];
    do {
        m = *--p;
        *p = (Pos)(m >= wsize ? m - wsize : NIL);
        /* If n is not on any hash chain, prev[n] is garbage but
         * its value will never be used.
         */
    } while (--n);
#endif
}

/* ========================================================================= */
int ZEXPORT deflateInit_(z_stream *strm, int level, const char *version, int stream_size) {
    return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, version, stream_size);
    /* Todo: ignore strm->next_in if we use it as window */
}

/* ========================================================================= */
int ZEXPORT deflateInit2_(z_stream *strm, int level, int method, int windowBits,
                           int memLevel, int strategy, const char *version, int stream_size) {
    unsigned window_padding = 0;
    deflate_state *s;
    int wrap = 1;
    static const char my_version[] = ZLIB_VERSION;

    uint16_t *overlay;
    /* We overlay pending_buf and d_buf+l_buf. This works since the average
     * output size for (length,distance) codes is <= 24 bits.
     */

#ifdef X86_CPUID
    x86_check_features();
#endif

    if (version == NULL || version[0] != my_version[0] || stream_size != sizeof(z_stream)) {
        return Z_VERSION_ERROR;
    }
    if (strm == NULL)
        return Z_STREAM_ERROR;

    strm->msg = NULL;
    if (strm->zalloc == NULL) {
        strm->zalloc = zcalloc;
        strm->opaque = NULL;
    }
    if (strm->zfree == NULL)
        strm->zfree = zcfree;

    if (level == Z_DEFAULT_COMPRESSION)
        level = 6;

    if (windowBits < 0) { /* suppress zlib wrapper */
        wrap = 0;
        windowBits = -windowBits;
#ifdef GZIP
    } else if (windowBits > 15) {
        wrap = 2;       /* write gzip wrapper instead */
        windowBits -= 16;
#endif
    }
    if (memLevel < 1 || memLevel > MAX_MEM_LEVEL || method != Z_DEFLATED || windowBits < 8 ||
        windowBits > 15 || level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED ||
        (windowBits == 8 && wrap != 1)) {
        return Z_STREAM_ERROR;
    }
    if (windowBits == 8)
        windowBits = 9;  /* until 256-byte window bug fixed */

#ifdef X86_QUICK_STRATEGY
    if (level == 1)
        windowBits = 13;
#endif

    s = (deflate_state *) ZALLOC(strm, 1, sizeof(deflate_state));
    if (s == NULL)
        return Z_MEM_ERROR;
    strm->state = (struct internal_state *)s;
    s->strm = strm;
    s->status = INIT_STATE;     /* to pass state test in deflateReset() */

    s->wrap = wrap;
    s->gzhead = NULL;
    s->w_bits = (unsigned int)windowBits;
    s->w_size = 1 << s->w_bits;
    s->w_mask = s->w_size - 1;

#ifdef X86_SSE4_2_CRC_HASH
    if (x86_cpu_has_sse42)
        s->hash_bits = (unsigned int)15;
    else
#endif
        s->hash_bits = (unsigned int)memLevel + 7;

    s->hash_size = 1 << s->hash_bits;
    s->hash_mask = s->hash_size - 1;
    s->hash_shift =  ((s->hash_bits+MIN_MATCH-1)/MIN_MATCH);

#ifdef X86_PCLMULQDQ_CRC
    window_padding = 8;
#endif

    s->window = (unsigned char *) ZALLOC(strm, s->w_size + window_padding, 2*sizeof(unsigned char));
    s->prev   = (Pos *)  ZALLOC(strm, s->w_size, sizeof(Pos));
    s->head   = (Pos *)  ZALLOC(strm, s->hash_size, sizeof(Pos));

    s->high_water = 0;      /* nothing written to s->window yet */

    s->lit_bufsize = 1 << (memLevel + 6); /* 16K elements by default */

    overlay = (uint16_t *) ZALLOC(strm, s->lit_bufsize, sizeof(uint16_t)+2);
    s->pending_buf = (unsigned char *) overlay;
    s->pending_buf_size = (unsigned long)s->lit_bufsize * (sizeof(uint16_t)+2L);

    if (s->window == NULL || s->prev == NULL || s->head == NULL ||
        s->pending_buf == NULL) {
        s->status = FINISH_STATE;
        strm->msg = ERR_MSG(Z_MEM_ERROR);
        deflateEnd(strm);
        return Z_MEM_ERROR;
    }
    s->d_buf = overlay + s->lit_bufsize/sizeof(uint16_t);
    s->l_buf = s->pending_buf + (1+sizeof(uint16_t))*s->lit_bufsize;

    s->level = level;
    s->strategy = strategy;
    s->method = (unsigned char)method;
    s->block_open = 0;

    return deflateReset(strm);
}

/* =========================================================================
 * Check for a valid deflate stream state. Return 0 if ok, 1 if not.
 */
static int deflateStateCheck (z_stream *strm) {
    deflate_state *s;
    if (strm == NULL ||
        strm->zalloc == (alloc_func)0 || strm->zfree == (free_func)0)
        return 1;
    s = strm->state;
    if (s == NULL || s->strm != strm || (s->status != INIT_STATE &&
#ifdef GZIP
                                           s->status != GZIP_STATE &&
#endif
                                           s->status != EXTRA_STATE &&
                                           s->status != NAME_STATE &&
                                           s->status != COMMENT_STATE &&
                                           s->status != HCRC_STATE &&
                                           s->status != BUSY_STATE &&
                                           s->status != FINISH_STATE))
        return 1;
    return 0;
}

/* ========================================================================= */
int ZEXPORT deflateSetDictionary(z_stream *strm, const unsigned char *dictionary, unsigned int dictLength) {
    deflate_state *s;
    unsigned int str, n;
    int wrap;
    uint32_t avail;
    const unsigned char *next;

    if (deflateStateCheck(strm) || dictionary == NULL)
        return Z_STREAM_ERROR;
    s = strm->state;
    wrap = s->wrap;
    if (wrap == 2 || (wrap == 1 && s->status != INIT_STATE) || s->lookahead)
        return Z_STREAM_ERROR;

    /* when using zlib wrappers, compute Adler-32 for provided dictionary */
    if (wrap == 1)
        strm->adler = adler32(strm->adler, dictionary, dictLength);
    s->wrap = 0;                    /* avoid computing Adler-32 in read_buf */

    /* if dictionary would fill window, just replace the history */
    if (dictLength >= s->w_size) {
        if (wrap == 0) {            /* already empty otherwise */
            CLEAR_HASH(s);
            s->strstart = 0;
            s->block_start = 0L;
            s->insert = 0;
        }
        dictionary += dictLength - s->w_size;  /* use the tail */
        dictLength = s->w_size;
    }

    /* insert dictionary into window and hash */
    avail = strm->avail_in;
    next = strm->next_in;
    strm->avail_in = dictLength;
    strm->next_in = (const unsigned char *)dictionary;
    fill_window(s);
    while (s->lookahead >= MIN_MATCH) {
        str = s->strstart;
        n = s->lookahead - (MIN_MATCH-1);
        insert_string(s, str, n);
        s->strstart = str + n;
        s->lookahead = MIN_MATCH-1;
        fill_window(s);
    }
    s->strstart += s->lookahead;
    s->block_start = (long)s->strstart;
    s->insert = s->lookahead;
    s->lookahead = 0;
    s->match_length = s->prev_length = MIN_MATCH-1;
    s->match_available = 0;
    strm->next_in = next;
    strm->avail_in = avail;
    s->wrap = wrap;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateGetDictionary (z_stream *strm, unsigned char *dictionary, unsigned int  *dictLength) {
    deflate_state *s;
    unsigned int len;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    s = strm->state;
    len = s->strstart + s->lookahead;
    if (len > s->w_size)
        len = s->w_size;
    if (dictionary != NULL && len)
        memcpy(dictionary, s->window + s->strstart + s->lookahead - len, len);
    if (dictLength != NULL)
        *dictLength = len;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateResetKeep(z_stream *strm) {
    deflate_state *s;

    if (deflateStateCheck(strm)) {
        return Z_STREAM_ERROR;
    }

    strm->total_in = strm->total_out = 0;
    strm->msg = NULL; /* use zfree if we ever allocate msg dynamically */
    strm->data_type = Z_UNKNOWN;

    s = (deflate_state *)strm->state;
    s->pending = 0;
    s->pending_out = s->pending_buf;

    if (s->wrap < 0) {
        s->wrap = -s->wrap; /* was made negative by deflate(..., Z_FINISH); */
    }
    s->status =
#ifdef GZIP
        s->wrap == 2 ? GZIP_STATE :
#endif
        s->wrap ? INIT_STATE : BUSY_STATE;

#ifdef GZIP
    if (s->wrap == 2)
        crc_reset(s);
    else
#endif
        strm->adler = adler32(0L, NULL, 0);
    s->last_flush = Z_NO_FLUSH;

    _tr_init(s);

    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateReset(z_stream *strm) {
    int ret;

    ret = deflateResetKeep(strm);
    if (ret == Z_OK)
        lm_init(strm->state);
    return ret;
}

/* ========================================================================= */
int ZEXPORT deflateSetHeader(z_stream *strm, gz_headerp head) {
    if (deflateStateCheck(strm) || strm->state->wrap != 2)
        return Z_STREAM_ERROR;
    strm->state->gzhead = head;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflatePending(z_stream *strm, uint32_t *pending, int *bits) {
    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    if (pending != NULL)
        *pending = strm->state->pending;
    if (bits != NULL)
        *bits = strm->state->bi_valid;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflatePrime(z_stream *strm, int bits, int value) {
    deflate_state *s;
    int put;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    s = strm->state;
    if ((unsigned char *)(s->d_buf) < s->pending_out + ((Buf_size + 7) >> 3))
        return Z_BUF_ERROR;
    do {
        put = Buf_size - s->bi_valid;
        if (put > bits)
            put = bits;
        s->bi_buf |= (uint16_t)((value & ((1 << put) - 1)) << s->bi_valid);
        s->bi_valid += put;
        _tr_flush_bits(s);
        value >>= put;
        bits -= put;
    } while (bits);
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateParams(z_stream *strm, int level, int strategy) {
    deflate_state *s;
    compress_func func;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    s = strm->state;

    if (level == Z_DEFAULT_COMPRESSION)
        level = 6;
    if (level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
        return Z_STREAM_ERROR;
    }
    func = configuration_table[s->level].func;

    if ((strategy != s->strategy || func != configuration_table[level].func) && s->high_water) {
        /* Flush the last buffer: */
        int err = deflate(strm, Z_BLOCK);
        if (err == Z_STREAM_ERROR)
            return err;
        if (strm->avail_out == 0)
            return Z_BUF_ERROR;
    }
    if (s->level != level) {
        if (s->level == 0 && s->matches != 0) {
            if (s->matches == 1) {
                slide_hash(s);
            } else {
                CLEAR_HASH(s);
            }
            s->matches = 0;
        }
        s->level = level;
        s->max_lazy_match   = configuration_table[level].max_lazy;
        s->good_match       = configuration_table[level].good_length;
        s->nice_match       = configuration_table[level].nice_length;
        s->max_chain_length = configuration_table[level].max_chain;
    }
    s->strategy = strategy;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateTune(z_stream *strm, int good_length, int max_lazy, int nice_length, int max_chain) {
    deflate_state *s;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    s = strm->state;
    s->good_match = (unsigned int)good_length;
    s->max_lazy_match = (unsigned int)max_lazy;
    s->nice_match = nice_length;
    s->max_chain_length = (unsigned int)max_chain;
    return Z_OK;
}

/* =========================================================================
 * For the default windowBits of 15 and memLevel of 8, this function returns
 * a close to exact, as well as small, upper bound on the compressed size.
 * They are coded as constants here for a reason--if the #define's are
 * changed, then this function needs to be changed as well.  The return
 * value for 15 and 8 only works for those exact settings.
 *
 * For any setting other than those defaults for windowBits and memLevel,
 * the value returned is a conservative worst case for the maximum expansion
 * resulting from using fixed blocks instead of stored blocks, which deflate
 * can emit on compressed data for some combinations of the parameters.
 *
 * This function could be more sophisticated to provide closer upper bounds for
 * every combination of windowBits and memLevel.  But even the conservative
 * upper bound of about 14% expansion does not seem onerous for output buffer
 * allocation.
 */
unsigned long ZEXPORT deflateBound(z_stream *strm, unsigned long sourceLen) {
    deflate_state *s;
    unsigned long complen, wraplen;

    /* conservative upper bound for compressed data */
    complen = sourceLen + ((sourceLen + 7) >> 3) + ((sourceLen + 63) >> 6) + 5;

    /* if can't get parameters, return conservative bound plus zlib wrapper */
    if (deflateStateCheck(strm))
        return complen + 6;

    /* compute wrapper length */
    s = strm->state;
    switch (s->wrap) {
    case 0:                                 /* raw deflate */
        wraplen = 0;
        break;
    case 1:                                 /* zlib wrapper */
        wraplen = 6 + (s->strstart ? 4 : 0);
        break;
#ifdef GZIP
    case 2:                                 /* gzip wrapper */
        wraplen = 18;
        if (s->gzhead != NULL) {            /* user-supplied gzip header */
            unsigned char *str;
            if (s->gzhead->extra != NULL) {
                wraplen += 2 + s->gzhead->extra_len;
            }
            str = s->gzhead->name;
            if (str != NULL) {
                do {
                    wraplen++;
                } while (*str++);
            }
            str = s->gzhead->comment;
            if (str != NULL) {
                do {
                    wraplen++;
                } while (*str++);
            }
            if (s->gzhead->hcrc)
                wraplen += 2;
        }
        break;
#endif
    default:                                /* for compiler happiness */
        wraplen = 6;
    }

    /* if not default parameters, return conservative bound */
    if (s->w_bits != 15 || s->hash_bits != 8 + 7)
        return complen + wraplen;

    /* default settings: return tight bound for that case */
    return sourceLen + (sourceLen >> 12) + (sourceLen >> 14) + (sourceLen >> 25) + 13 - 6 + wraplen;
}

/* =========================================================================
 * Put a short in the pending buffer. The 16-bit value is put in MSB order.
 * IN assertion: the stream state is correct and there is enough room in
 * pending_buf.
 */
static void putShortMSB(deflate_state *s, uint16_t b) {
    put_byte(s, (unsigned char)(b >> 8));
    put_byte(s, (unsigned char)(b & 0xff));
}

/* =========================================================================
 * Flush as much pending output as possible. All deflate() output, except for
 * some deflate_stored() output, goes through this function so some
 * applications may wish to modify it to avoid allocating a large
 * strm->next_out buffer and copying into it. (See also read_buf()).
 */
ZLIB_INTERNAL void flush_pending(z_stream *strm) {
    uint32_t len;
    deflate_state *s = strm->state;

    _tr_flush_bits(s);
    len = s->pending;
    if (len > strm->avail_out)
        len = strm->avail_out;
    if (len == 0)
        return;

    memcpy(strm->next_out, s->pending_out, len);
    strm->next_out  += len;
    s->pending_out  += len;
    strm->total_out += len;
    strm->avail_out -= len;
    s->pending      -= len;
    if (s->pending == 0) {
        s->pending_out = s->pending_buf;
    }
}

/* ===========================================================================
 * Update the header CRC with the bytes s->pending_buf[beg..s->pending - 1].
 */
#define HCRC_UPDATE(beg) \
    do { \
        if (s->gzhead->hcrc && s->pending > (beg)) \
            strm->adler = crc32(strm->adler, s->pending_buf + (beg), s->pending - (beg)); \
    } while (0)

/* ========================================================================= */
int ZEXPORT deflate(z_stream *strm, int flush) {
    int old_flush; /* value of flush param for previous deflate call */
    deflate_state *s;

    if (deflateStateCheck(strm) || flush > Z_BLOCK || flush < 0) {
        return Z_STREAM_ERROR;
    }
    s = strm->state;

    if (strm->next_out == NULL || (strm->avail_in != 0 && strm->next_in == NULL) ||
        (s->status == FINISH_STATE && flush != Z_FINISH)) {
        ERR_RETURN(strm, Z_STREAM_ERROR);
    }
    if (strm->avail_out == 0)
        ERR_RETURN(strm, Z_BUF_ERROR);

    s->strm = strm; /* just in case */
    old_flush = s->last_flush;
    s->last_flush = flush;

    /* Flush as much pending output as possible */
    if (s->pending != 0) {
        flush_pending(strm);
        if (strm->avail_out == 0) {
            /* Since avail_out is 0, deflate will be called again with
             * more output space, but possibly with both pending and
             * avail_in equal to zero. There won't be anything to do,
             * but this is not an error situation so make sure we
             * return OK instead of BUF_ERROR at next call of deflate:
             */
            s->last_flush = -1;
            return Z_OK;
        }

    /* Make sure there is something to do and avoid duplicate consecutive
     * flushes. For repeated and useless calls with Z_FINISH, we keep
     * returning Z_STREAM_END instead of Z_BUF_ERROR.
     */
    } else if (strm->avail_in == 0 && RANK(flush) <= RANK(old_flush) &&
               flush != Z_FINISH) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* User must not provide more input after the first FINISH: */
    if (s->status == FINISH_STATE && strm->avail_in != 0) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* Write the header */
    if (s->status == INIT_STATE) {
        /* zlib header */
        unsigned int header = (Z_DEFLATED + ((s->w_bits-8)<<4)) << 8;
        unsigned int level_flags;

        if (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2)
            level_flags = 0;
        else if (s->level < TRIGGER_LEVEL)
            level_flags = 1;
        else if (s->level == TRIGGER_LEVEL)
            level_flags = 2;
        else
            level_flags = 3;
        header |= (level_flags << 6);
        if (s->strstart != 0) header |= PRESET_DICT;
        header += 31 - (header % 31);

        putShortMSB(s, header);

        /* Save the adler32 of the preset dictionary: */
        if (s->strstart != 0) {
            putShortMSB(s, (uint16_t)(strm->adler >> 16));
            putShortMSB(s, (uint16_t)(strm->adler));
        }
        strm->adler = adler32(0L, NULL, 0);
        s->status = BUSY_STATE;

        /* Compression must start with an empty pending buffer */
        flush_pending(strm);
        if (s->pending != 0) {
            s->last_flush = -1;
            return Z_OK;
        }
    }
#ifdef GZIP
    if (s->status == GZIP_STATE) {
        /* gzip header */
        crc_reset(s);
        put_byte(s, 31);
        put_byte(s, 139);
        put_byte(s, 8);
        if (s->gzhead == NULL) {
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, s->level == 9 ? 2 :
                     (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
            put_byte(s, OS_CODE);
            s->status = BUSY_STATE;

            /* Compression must start with an empty pending buffer */
            flush_pending(strm);
            if (s->pending != 0) {
                s->last_flush = -1;
                return Z_OK;
            }
        }
        else {
            put_byte(s, (s->gzhead->text ? 1 : 0) +
                     (s->gzhead->hcrc ? 2 : 0) +
                     (s->gzhead->extra == NULL ? 0 : 4) +
                     (s->gzhead->name == NULL ? 0 : 8) +
                     (s->gzhead->comment == NULL ? 0 : 16)
                     );
            put_byte(s, (unsigned char)(s->gzhead->time & 0xff));
            put_byte(s, (unsigned char)((s->gzhead->time >> 8) & 0xff));
            put_byte(s, (unsigned char)((s->gzhead->time >> 16) & 0xff));
            put_byte(s, (unsigned char)((s->gzhead->time >> 24) & 0xff));
            put_byte(s, s->level == 9 ? 2 :
                     (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ? 4 : 0));
            put_byte(s, s->gzhead->os & 0xff);
            if (s->gzhead->extra != NULL) {
                put_byte(s, s->gzhead->extra_len & 0xff);
                put_byte(s, (s->gzhead->extra_len >> 8) & 0xff);
            }
            if (s->gzhead->hcrc)
                strm->adler = crc32(strm->adler, s->pending_buf, s->pending);
            s->gzindex = 0;
            s->status = EXTRA_STATE;
        }
    }
    if (s->status == EXTRA_STATE) {
        if (s->gzhead->extra != NULL) {
            uint32_t beg = s->pending;   /* start of bytes to update crc */
            uint32_t left = (s->gzhead->extra_len & 0xffff) - s->gzindex;

            while (s->pending + left > s->pending_buf_size) {
                uint32_t copy = s->pending_buf_size - s->pending;
                memcpy(s->pending_buf + s->pending, s->gzhead->extra + s->gzindex, copy);
                s->pending = s->pending_buf_size;
                HCRC_UPDATE(beg);
                s->gzindex += copy;
                flush_pending(strm);
                if (s->pending != 0) {
                    s->last_flush = -1;
                    return Z_OK;
                }
                beg = 0;
                left -= copy;
            }
            memcpy(s->pending_buf + s->pending, s->gzhead->extra + s->gzindex, left);
            s->pending += left;
            HCRC_UPDATE(beg);
            s->gzindex = 0;
        }
        s->status = NAME_STATE;
    }
    if (s->status == NAME_STATE) {
        if (s->gzhead->name != NULL) {
            uint32_t beg = s->pending;   /* start of bytes to update crc */
            int val;

            do {
                if (s->pending == s->pending_buf_size) {
                    HCRC_UPDATE(beg);
                    flush_pending(strm);
                    if (s->pending != 0) {
                        s->last_flush = -1;
                        return Z_OK;
                    }
                    beg = 0;
                }
                val = s->gzhead->name[s->gzindex++];
                put_byte(s, val);
            } while (val != 0);
            HCRC_UPDATE(beg);
            s->gzindex = 0;
        }
        s->status = COMMENT_STATE;
    }
    if (s->status == COMMENT_STATE) {
        if (s->gzhead->comment != NULL) {
            uint32_t beg = s->pending;  /* start of bytes to update crc */
            int val;

            do {
                if (s->pending == s->pending_buf_size) {
                    HCRC_UPDATE(beg);
                    flush_pending(strm);
                    if (s->pending != 0) {
                        s->last_flush = -1;
                        return Z_OK;
                    }
                    beg = 0;
                }
                val = s->gzhead->comment[s->gzindex++];
                put_byte(s, val);
            } while (val != 0);
            HCRC_UPDATE(beg);
        }
        s->status = HCRC_STATE;
    }
    if (s->status == HCRC_STATE) {
        if (s->gzhead->hcrc) {
            if (s->pending + 2 > s->pending_buf_size) {
                flush_pending(strm);
                if (s->pending != 0) {
                    s->last_flush = -1;
                    return Z_OK;
                }
            }
            put_byte(s, (unsigned char)(strm->adler & 0xff));
            put_byte(s, (unsigned char)((strm->adler >> 8) & 0xff));
            crc_reset(s);
        }
        s->status = BUSY_STATE;

        /* Compression must start with an empty pending buffer */
        flush_pending(strm);
        if (s->pending != 0) {
            s->last_flush = -1;
            return Z_OK;
        }
    }
#endif

    /* Start a new block or continue the current one.
     */
    if (strm->avail_in != 0 || s->lookahead != 0 || (flush != Z_NO_FLUSH && s->status != FINISH_STATE)) {
        block_state bstate;

#ifdef X86_QUICK_STRATEGY
        if (s->level == 1 && !x86_cpu_has_sse42)
            bstate = s->strategy == Z_HUFFMAN_ONLY ? deflate_huff(s, flush) :
                    (s->strategy == Z_RLE ? deflate_rle(s, flush) : deflate_fast(s, flush));
        else
#endif
            bstate = s->level == 0 ? deflate_stored(s, flush) : 
                     s->strategy == Z_HUFFMAN_ONLY ? deflate_huff(s, flush) :
                    (s->strategy == Z_RLE ? deflate_rle(s, flush) : (*(configuration_table[s->level].func))(s, flush));

        if (bstate == finish_started || bstate == finish_done) {
            s->status = FINISH_STATE;
        }
        if (bstate == need_more || bstate == finish_started) {
            if (strm->avail_out == 0) {
                s->last_flush = -1; /* avoid BUF_ERROR next call, see above */
            }
            return Z_OK;
            /* If flush != Z_NO_FLUSH && avail_out == 0, the next call
             * of deflate should use the same flush parameter to make sure
             * that the flush is complete. So we don't have to output an
             * empty block here, this will be done at next call. This also
             * ensures that for a very small output buffer, we emit at most
             * one empty block.
             */
        }
        if (bstate == block_done) {
            if (flush == Z_PARTIAL_FLUSH) {
                _tr_align(s);
            } else if (flush != Z_BLOCK) { /* FULL_FLUSH or SYNC_FLUSH */
                _tr_stored_block(s, (char*)0, 0L, 0);
                /* For a full flush, this empty block will be recognized
                 * as a special marker by inflate_sync().
                 */
                if (flush == Z_FULL_FLUSH) {
                    CLEAR_HASH(s);             /* forget history */
                    if (s->lookahead == 0) {
                        s->strstart = 0;
                        s->block_start = 0L;
                        s->insert = 0;
                    }
                }
            }
            flush_pending(strm);
            if (strm->avail_out == 0) {
              s->last_flush = -1; /* avoid BUF_ERROR at next call, see above */
              return Z_OK;
            }
        }
    }

    if (flush != Z_FINISH)
        return Z_OK;
    if (s->wrap <= 0)
        return Z_STREAM_END;

    /* Write the trailer */
#ifdef GZIP
    if (s->wrap == 2) {
#  ifdef X86_PCLMULQDQ_CRC
        crc_finalize(s);
#  endif
        put_byte(s, (unsigned char)(strm->adler & 0xff));
        put_byte(s, (unsigned char)((strm->adler >> 8) & 0xff));
        put_byte(s, (unsigned char)((strm->adler >> 16) & 0xff));
        put_byte(s, (unsigned char)((strm->adler >> 24) & 0xff));
        put_byte(s, (unsigned char)(strm->total_in & 0xff));
        put_byte(s, (unsigned char)((strm->total_in >> 8) & 0xff));
        put_byte(s, (unsigned char)((strm->total_in >> 16) & 0xff));
        put_byte(s, (unsigned char)((strm->total_in >> 24) & 0xff));
    } else
#endif
    {
        putShortMSB(s, (uint16_t)(strm->adler >> 16));
        putShortMSB(s, (uint16_t)strm->adler);
    }
    flush_pending(strm);
    /* If avail_out is zero, the application will call deflate again
     * to flush the rest.
     */
    if (s->wrap > 0)
        s->wrap = -s->wrap; /* write the trailer only once! */
    return s->pending != 0 ? Z_OK : Z_STREAM_END;
}

/* ========================================================================= */
int ZEXPORT deflateEnd(z_stream *strm) {
    int status;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;

    status = strm->state->status;

    /* Deallocate in reverse order of allocations: */
    TRY_FREE(strm, strm->state->pending_buf);
    TRY_FREE(strm, strm->state->head);
    TRY_FREE(strm, strm->state->prev);
    TRY_FREE(strm, strm->state->window);

    ZFREE(strm, strm->state);
    strm->state = NULL;

    return status == BUSY_STATE ? Z_DATA_ERROR : Z_OK;
}

/* =========================================================================
 * Copy the source state to the destination state.
 */
int ZEXPORT deflateCopy(z_stream *dest, z_stream *source) {
    deflate_state *ds;
    deflate_state *ss;
    uint16_t *overlay;

    if (deflateStateCheck(source) || dest == NULL) {
        return Z_STREAM_ERROR;
    }

    ss = source->state;

    memcpy((void *)dest, (void *)source, sizeof(z_stream));

    ds = (deflate_state *) ZALLOC(dest, 1, sizeof(deflate_state));
    if (ds == NULL)
        return Z_MEM_ERROR;
    dest->state = (struct internal_state *) ds;
    memcpy((void *)ds, (void *)ss, sizeof(deflate_state));
    ds->strm = dest;

    ds->window = (unsigned char *) ZALLOC(dest, ds->w_size, 2*sizeof(unsigned char));
    ds->prev   = (Pos *)  ZALLOC(dest, ds->w_size, sizeof(Pos));
    ds->head   = (Pos *)  ZALLOC(dest, ds->hash_size, sizeof(Pos));
    overlay = (uint16_t *) ZALLOC(dest, ds->lit_bufsize, sizeof(uint16_t)+2);
    ds->pending_buf = (unsigned char *) overlay;

    if (ds->window == NULL || ds->prev == NULL || ds->head == NULL || ds->pending_buf == NULL) {
        deflateEnd(dest);
        return Z_MEM_ERROR;
    }

    memcpy(ds->window, ss->window, ds->w_size * 2 * sizeof(unsigned char));
    memcpy((void *)ds->prev, (void *)ss->prev, ds->w_size * sizeof(Pos));
    memcpy((void *)ds->head, (void *)ss->head, ds->hash_size * sizeof(Pos));
    memcpy(ds->pending_buf, ss->pending_buf, (unsigned int)ds->pending_buf_size);

    ds->pending_out = ds->pending_buf + (ss->pending_out - ss->pending_buf);
    ds->d_buf = overlay + ds->lit_bufsize/sizeof(uint16_t);
    ds->l_buf = ds->pending_buf + (1+sizeof(uint16_t))*ds->lit_bufsize;

    ds->l_desc.dyn_tree = ds->dyn_ltree;
    ds->d_desc.dyn_tree = ds->dyn_dtree;
    ds->bl_desc.dyn_tree = ds->bl_tree;

    return Z_OK;
}

/* ===========================================================================
 * Read a new buffer from the current input stream, update the adler32
 * and total number of bytes read.  All deflate() input goes through
 * this function so some applications may wish to modify it to avoid
 * allocating a large strm->next_in buffer and copying from it.
 * (See also flush_pending()).
 */
ZLIB_INTERNAL unsigned read_buf(z_stream *strm, unsigned char *buf, unsigned size) {
    uint32_t len = strm->avail_in;

    if (len > size)
        len = size;
    if (len == 0)
        return 0;

    strm->avail_in  -= len;

#ifdef GZIP
    if (strm->state->wrap == 2)
        copy_with_crc(strm, buf, len);
    else
#endif
    {
        memcpy(buf, strm->next_in, len);
        if (strm->state->wrap == 1)
            strm->adler = adler32(strm->adler, buf, len);
    }
    strm->next_in  += len;
    strm->total_in += len;

    return len;
}

/* ===========================================================================
 * Initialize the "longest match" routines for a new zlib stream
 */
static void lm_init(deflate_state *s) {
    s->window_size = (unsigned long)2L*s->w_size;

    CLEAR_HASH(s);

    /* Set the default configuration parameters:
     */
    s->max_lazy_match   = configuration_table[s->level].max_lazy;
    s->good_match       = configuration_table[s->level].good_length;
    s->nice_match       = configuration_table[s->level].nice_length;
    s->max_chain_length = configuration_table[s->level].max_chain;

    s->strstart = 0;
    s->block_start = 0L;
    s->lookahead = 0;
    s->insert = 0;
    s->match_length = s->prev_length = MIN_MATCH-1;
    s->match_available = 0;
    s->ins_h = 0;
}

#ifdef ZLIB_DEBUG
#define EQUAL 0
/* result of memcmp for equal strings */

/* ===========================================================================
 * Check that the match at match_start is indeed a match.
 */
void check_match(deflate_state *s, IPos start, IPos match, int length) {
    /* check that the match is indeed a match */
    if (memcmp(s->window + match, s->window + start, length) != EQUAL) {
        fprintf(stderr, " start %u, match %u, length %d\n", start, match, length);
        do {
            fprintf(stderr, "%c%c", s->window[match++], s->window[start++]);
        } while (--length != 0);
        z_error("invalid match");
    }
    if (z_verbose > 1) {
        fprintf(stderr, "\\[%u,%d]", start-match, length);
        do {
            putc(s->window[start++], stderr);
        } while (--length != 0);
    }
}
#else
#  define check_match(s, start, match, length)
#endif /* ZLIB_DEBUG */

/* ===========================================================================
 * Fill the window when the lookahead becomes insufficient.
 * Updates strstart and lookahead.
 *
 * IN assertion: lookahead < MIN_LOOKAHEAD
 * OUT assertions: strstart <= window_size-MIN_LOOKAHEAD
 *    At least one byte has been read, or avail_in == 0; reads are
 *    performed for at least two bytes (required for the zip translate_eol
 *    option -- not supported here).
 */
#ifdef X86_SSE2_FILL_WINDOW
extern void fill_window_sse(deflate_state *s);
#endif
void fill_window_c(deflate_state *s);

void fill_window(deflate_state *s) {
#ifdef X86_SSE2_FILL_WINDOW
# ifndef X86_NOCHECK_SSE2
    if (x86_cpu_has_sse2) {
# endif
        fill_window_sse(s);
# ifndef X86_NOCHECK_SSE2
    } else {
        fill_window_c(s);
    }
# endif

#else
    fill_window_c(s);
#endif
}

void fill_window_c(deflate_state *s) {
    unsigned n;
    unsigned long more;  /* Amount of free space at the end of the window. */
    unsigned int wsize = s->w_size;

    Assert(s->lookahead < MIN_LOOKAHEAD, "already enough lookahead");

    do {
        more = s->window_size - s->lookahead - s->strstart;

        /* If the window is almost full and there is insufficient lookahead,
         * move the upper half to the lower one to make room in the upper half.
         */
        if (s->strstart >= wsize+MAX_DIST(s)) {
            memcpy(s->window, s->window+wsize, wsize - more);
            s->match_start -= wsize;
            s->strstart    -= wsize; /* we now have strstart >= MAX_DIST */
            s->block_start -= (long) wsize;

            slide_hash(s);
            more += wsize;
        }
        if (s->strm->avail_in == 0)
            break;

        /* If there was no sliding:
         *    strstart <= WSIZE+MAX_DIST-1 && lookahead <= MIN_LOOKAHEAD - 1 &&
         *    more == window_size - lookahead - strstart
         * => more >= window_size - (MIN_LOOKAHEAD-1 + WSIZE + MAX_DIST-1)
         * => more >= window_size - 2*WSIZE + 2
         * In the BIG_MEM or MMAP case (not yet supported),
         *   window_size == input_size + MIN_LOOKAHEAD  &&
         *   strstart + s->lookahead <= input_size => more >= MIN_LOOKAHEAD.
         * Otherwise, window_size == 2*WSIZE so more >= 2.
         * If there was sliding, more >= WSIZE. So in all cases, more >= 2.
         */
        Assert(more >= 2, "more < 2");

        n = read_buf(s->strm, s->window + s->strstart + s->lookahead, more);
        s->lookahead += n;

        /* Initialize the hash value now that we have some input: */
        if (s->lookahead + s->insert >= MIN_MATCH) {
            unsigned int str = s->strstart - s->insert;
            s->ins_h = s->window[str];
#ifndef NOT_TWEAK_COMPILER
            {
                unsigned int insert_cnt = s->insert;
                unsigned int slen;
                if (unlikely(s->lookahead < MIN_MATCH))
                    insert_cnt += s->lookahead - MIN_MATCH;
                slen = insert_cnt;
                if (str >= (MIN_MATCH - 2))
                {
                    str += 2 - MIN_MATCH;
                    insert_cnt += MIN_MATCH - 2;
                }
                if (insert_cnt > 0)
                {
                    insert_string(s, str, insert_cnt);
                    s->insert -= slen;
                }
            }
#else
            if (str >= 1)
                insert_string(s, str + 2 - MIN_MATCH, 1);
#if MIN_MATCH != 3
#warning    Call insert_string() MIN_MATCH-3 more times
#endif
            while (s->insert) {
                insert_string(s, str, 1);
                str++;
                s->insert--;
                if (s->lookahead + s->insert < MIN_MATCH)
                    break;
            }
#endif
        }
        /* If the whole input has less than MIN_MATCH bytes, ins_h is garbage,
         * but this is not important since only literal bytes will be emitted.
         */
    } while (s->lookahead < MIN_LOOKAHEAD && s->strm->avail_in != 0);

    /* If the WIN_INIT bytes after the end of the current data have never been
     * written, then zero those bytes in order to avoid memory check reports of
     * the use of uninitialized (or uninitialised as Julian writes) bytes by
     * the longest match routines.  Update the high water mark for the next
     * time through here.  WIN_INIT is set to MAX_MATCH since the longest match
     * routines allow scanning to strstart + MAX_MATCH, ignoring lookahead.
     */
    if (s->high_water < s->window_size) {
        unsigned long curr = s->strstart + (unsigned long)(s->lookahead);
        unsigned long init;

        if (s->high_water < curr) {
            /* Previous high water mark below current data -- zero WIN_INIT
             * bytes or up to end of window, whichever is less.
             */
            init = s->window_size - curr;
            if (init > WIN_INIT)
                init = WIN_INIT;
            memset(s->window + curr, 0, init);
            s->high_water = curr + init;
        } else if (s->high_water < curr + WIN_INIT) {
            /* High water mark at or above current data, but below current data
             * plus WIN_INIT -- zero out to current data plus WIN_INIT, or up
             * to end of window, whichever is less.
             */
            init = curr + WIN_INIT;
            if (init > s->window_size)
                init = s->window_size;
            init -= s->high_water;
            memset(s->window + s->high_water, 0, init);
            s->high_water += init;
        }
    }

    Assert((unsigned long)s->strstart <= s->window_size - MIN_LOOKAHEAD,
           "not enough room for search");
}

/* ===========================================================================
 * Copy without compression as much as possible from the input stream, return
 * the current block state.
 *
 * In case deflateParams() is used to later switch to a non-zero compression
 * level, s->matches (otherwise unused when storing) keeps track of the number
 * of hash table slides to perform. If s->matches is 1, then one hash table
 * slide will be done when switching. If s->matches is 2, the maximum value
 * allowed here, then the hash table will be cleared, since two or more slides
 * is the same as a clear.
 *
 * deflate_stored() is written to minimize the number of times an input byte is
 * copied. It is most efficient with large input and output buffers, which
 * maximizes the opportunites to have a single copy from next_in to next_out.
 */
static block_state deflate_stored(deflate_state *s, int flush) {
    /* Smallest worthy block size when not flushing or finishing. By default
     * this is 32K. This can be as small as 507 bytes for memLevel == 1. For
     * large input and output buffers, the stored block size will be larger.
     */
    unsigned min_block = MIN(s->pending_buf_size - 5, s->w_size);

    /* Copy as many min_block or larger stored blocks directly to next_out as
     * possible. If flushing, copy the remaining available input to next_out as
     * stored blocks, if there is enough space.
     */
    unsigned len, left, have, last = 0;
    unsigned used = s->strm->avail_in;
    do {
        /* Set len to the maximum size block that we can copy directly with the
         * available input data and output space. Set left to how much of that
         * would be copied from what's left in the window.
         */
        len = MAX_STORED;       /* maximum deflate stored block length */
        have = (s->bi_valid + 42) >> 3;         /* number of header bytes */
        if (s->strm->avail_out < have)          /* need room for header */
            break;
            /* maximum stored block length that will fit in avail_out: */
        have = s->strm->avail_out - have;
        left = s->strstart - s->block_start;    /* bytes left in window */
        if (len > (unsigned long)left + s->strm->avail_in)
            len = left + s->strm->avail_in;     /* limit len to the input */
        if (len > have)
            len = have;                         /* limit len to the output */

        /* If the stored block would be less than min_block in length, or if
         * unable to copy all of the available input when flushing, then try
         * copying to the window and the pending buffer instead. Also don't
         * write an empty block when flushing -- deflate() does that.
         */
        if (len < min_block && ((len == 0 && flush != Z_FINISH) || flush == Z_NO_FLUSH || len != left + s->strm->avail_in))
            break;

        /* Make a dummy stored block in pending to get the header bytes,
         * including any pending bits. This also updates the debugging counts.
         */
        last = flush == Z_FINISH && len == left + s->strm->avail_in ? 1 : 0;
        _tr_stored_block(s, (char *)0, 0L, last);

        /* Replace the lengths in the dummy stored block with len. */
        s->pending_buf[s->pending - 4] = len;
        s->pending_buf[s->pending - 3] = len >> 8;
        s->pending_buf[s->pending - 2] = ~len;
        s->pending_buf[s->pending - 1] = ~len >> 8;

        /* Write the stored block header bytes. */
        flush_pending(s->strm);

#ifdef ZLIB_DEBUG
        /* Update debugging counts for the data about to be copied. */
        s->compressed_len += len << 3;
        s->bits_sent += len << 3;
#endif

        /* Copy uncompressed bytes from the window to next_out. */
        if (left) {
            if (left > len)
                left = len;
            memcpy(s->strm->next_out, s->window + s->block_start, left);
            s->strm->next_out += left;
            s->strm->avail_out -= left;
            s->strm->total_out += left;
            s->block_start += left;
            len -= left;
        }

        /* Copy uncompressed bytes directly from next_in to next_out, updating
         * the check value.
         */
        if (len) {
            read_buf(s->strm, s->strm->next_out, len);
            s->strm->next_out += len;
            s->strm->avail_out -= len;
            s->strm->total_out += len;
        }
    } while (last == 0);

    /* Update the sliding window with the last s->w_size bytes of the copied
     * data, or append all of the copied data to the existing window if less
     * than s->w_size bytes were copied. Also update the number of bytes to
     * insert in the hash tables, in the event that deflateParams() switches to
     * a non-zero compression level.
     */
    used -= s->strm->avail_in;      /* number of input bytes directly copied */
    if (used) {
        /* If any input was used, then no unused input remains in the window,
         * therefore s->block_start == s->strstart.
         */
        if (used >= s->w_size) {    /* supplant the previous history */
            s->matches = 2;         /* clear hash */
            memcpy(s->window, s->strm->next_in - s->w_size, s->w_size);
            s->strstart = s->w_size;
        }
        else {
            if (s->window_size - s->strstart <= used) {
                /* Slide the window down. */
                s->strstart -= s->w_size;
                memcpy(s->window, s->window + s->w_size, s->strstart);
                if (s->matches < 2)
                    s->matches++;   /* add a pending slide_hash() */
            }
            memcpy(s->window + s->strstart, s->strm->next_in - used, used);
            s->strstart += used;
        }
        s->block_start = s->strstart;
        s->insert += MIN(used, s->w_size - s->insert);
    }
    if (s->high_water < s->strstart)
        s->high_water = s->strstart;

    /* If the last block was written to next_out, then done. */
    if (last)
        return finish_done;

    /* If flushing and all input has been consumed, then done. */
    if (flush != Z_NO_FLUSH && flush != Z_FINISH &&
        s->strm->avail_in == 0 && (long)s->strstart == s->block_start)
        return block_done;

    /* Fill the window with any remaining input. */
    have = s->window_size - s->strstart - 1;
    if (s->strm->avail_in > have && s->block_start >= (long)s->w_size) {
        /* Slide the window down. */
        s->block_start -= s->w_size;
        s->strstart -= s->w_size;
        memcpy(s->window, s->window + s->w_size, s->strstart);
        if (s->matches < 2)
            s->matches++;           /* add a pending slide_hash() */
        have += s->w_size;          /* more space now */
    }
    if (have > s->strm->avail_in)
        have = s->strm->avail_in;
    if (have) {
        read_buf(s->strm, s->window + s->strstart, have);
        s->strstart += have;
    }
    if (s->high_water < s->strstart)
        s->high_water = s->strstart;

    /* There was not enough avail_out to write a complete worthy or flushed
     * stored block to next_out. Write a stored block to pending instead, if we
     * have enough input for a worthy block, or if flushing and there is enough
     * room for the remaining input as a stored block in the pending buffer.
     */
    have = (s->bi_valid + 42) >> 3;         /* number of header bytes */
        /* maximum stored block length that will fit in pending: */
    have = MIN(s->pending_buf_size - have, MAX_STORED);
    min_block = MIN(have, s->w_size);
    left = s->strstart - s->block_start;
    if (left >= min_block ||
        ((left || flush == Z_FINISH) && flush != Z_NO_FLUSH &&
         s->strm->avail_in == 0 && left <= have)) {
        len = MIN(left, have);
        last = flush == Z_FINISH && s->strm->avail_in == 0 &&
               len == left ? 1 : 0;
        _tr_stored_block(s, (charf *)s->window + s->block_start, len, last);
        s->block_start += len;
        flush_pending(s->strm);
    }

    /* We've done all we can with the available input and output. */
    return last ? finish_started : need_more;
}


/* ===========================================================================
 * For Z_RLE, simply look for runs of bytes, generate matches only of distance
 * one.  Do not maintain a hash table.  (It will be regenerated if this run of
 * deflate switches away from Z_RLE.)
 */
static block_state deflate_rle(deflate_state *s, int flush) {
    int bflush;                     /* set if current block must be flushed */
    unsigned int prev;              /* byte at distance one to match */
    unsigned char *scan, *strend;   /* scan goes up to strend for length of run */

    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the longest run, plus one for the unrolled loop.
         */
        if (s->lookahead <= MAX_MATCH) {
            fill_window(s);
            if (s->lookahead <= MAX_MATCH && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead == 0)
                break; /* flush the current block */
        }

        /* See how many times the previous byte repeats */
        s->match_length = 0;
        if (s->lookahead >= MIN_MATCH && s->strstart > 0) {
            scan = s->window + s->strstart - 1;
            prev = *scan;
            if (prev == *++scan && prev == *++scan && prev == *++scan) {
                strend = s->window + s->strstart + MAX_MATCH;
                do {
                } while (prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         scan < strend);
                s->match_length = MAX_MATCH - (unsigned int)(strend - scan);
                if (s->match_length > s->lookahead)
                    s->match_length = s->lookahead;
            }
            Assert(scan <= s->window+(unsigned int)(s->window_size-1), "wild scan");
        }

        /* Emit match if have run of MIN_MATCH or longer, else emit literal */
        if (s->match_length >= MIN_MATCH) {
            check_match(s, s->strstart, s->strstart - 1, s->match_length);

            _tr_tally_dist(s, 1, s->match_length - MIN_MATCH, bflush);

            s->lookahead -= s->match_length;
            s->strstart += s->match_length;
            s->match_length = 0;
        } else {
            /* No match, output a literal byte */
            Tracevv((stderr, "%c", s->window[s->strstart]));
            _tr_tally_lit(s, s->window[s->strstart], bflush);
            s->lookahead--;
            s->strstart++;
        }
        if (bflush)
            FLUSH_BLOCK(s, 0);
    }
    s->insert = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * For Z_HUFFMAN_ONLY, do not look for matches.  Do not maintain a hash table.
 * (It will be regenerated if this run of deflate switches away from Huffman.)
 */
static block_state deflate_huff(deflate_state *s, int flush) {
    int bflush;             /* set if current block must be flushed */

    for (;;) {
        /* Make sure that we have a literal to write. */
        if (s->lookahead == 0) {
            fill_window(s);
            if (s->lookahead == 0) {
                if (flush == Z_NO_FLUSH)
                    return need_more;
                break;      /* flush the current block */
            }
        }

        /* Output a literal byte */
        s->match_length = 0;
        Tracevv((stderr, "%c", s->window[s->strstart]));
        _tr_tally_lit(s, s->window[s->strstart], bflush);
        s->lookahead--;
        s->strstart++;
        if (bflush)
            FLUSH_BLOCK(s, 0);
    }
    s->insert = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->last_lit)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

#ifdef ZLIB_DEBUG
/* ===========================================================================
 * Send a value on a given number of bits.
 * IN assertion: length <= 16 and value fits in length bits.
 */
void send_bits(deflate_state *s, int value, int length) {
    Tracevv((stderr, " l %2d v %4x ", length, value));
    Assert(length > 0 && length <= 15, "invalid length");
    s->bits_sent += (unsigned long)length;

    /* If not enough room in bi_buf, use (valid) bits from bi_buf and
     * (16 - bi_valid) bits from value, leaving (width - (16-bi_valid))
     * unused bits in value.
     */
    if (s->bi_valid > (int)Buf_size - length) {
        s->bi_buf |= (uint16_t)value << s->bi_valid;
        put_short(s, s->bi_buf);
        s->bi_buf = (uint16_t)value >> (Buf_size - s->bi_valid);
        s->bi_valid += length - Buf_size;
    } else {
        s->bi_buf |= (uint16_t)value << s->bi_valid;
        s->bi_valid += length;
    }
}
#endif
