/*
 * Copyright (c) 2016-2019 Genome Research Ltd.
 * Author(s): James Bonfield
 * 
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions are met:
 * 
 *    1. Redistributions of source code must retain the above copyright notice,
 *       this list of conditions and the following disclaimer.
 * 
 *    2. Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 * 
 *    3. Neither the names Genome Research Ltd and Wellcome Trust Sanger
 *    Institute nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific
 *    prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY GENOME RESEARCH LTD AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL GENOME RESEARCH
 * LTD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// cc -I.. -g -O3 tokenise_name3.c arith_dynamic.c pooled_alloc.c -pthread -DTEST_TOKENISER

// As per tokenise_name2 but has the entropy encoder built in already,
// so we just have a single encode and decode binary.  (WIP; mainly TODO)

// TODO
//
// - Is it better when encoding 1, 2, 3, 3, 4, 5, 5, 6, 7, 9, 9, 10 to encode
//   this as a mixture of MATCH and DELTA ops, or as entirely as DELTA ops
//   with some delta values being zero?  I suspect the latter, but it is
//   not implemented here.  See "last_token_delta" comments in code.
//
// - Consider variable size string implementations.
//   Pascal style strings (length + str),
//   C style strings (nul terminated),
//   Or split blocks: length block and string contents block.
//
// - Is this one token-block or many serialised token-blocks?
//   A) Lots of different models but feeding one bit-buffer emitted to
//      by the entropy encoder => one block (fqzcomp).
//   B) Lots of different models each feeding their own bit-buffers
//      => many blocks.
//
// - multiple integer types depending on size; 1, 2, 4 byte long.
//
// - Consider token choice for isalnum instead of isalpha.  Sometimes better.
//
// - Consider token synchronisation (eg on matching chr symbols?) incase of
//   variable number.  Eg consider foo:0999, foo:1000, foo:1001 (the leading
//   zero adds an extra token).
//
// - Optimisation of tokens.  Eg:
//     HS25_09827:2:2102:11274:80442#49
//     HS25_09827:2:2109:12941:31311#49
//
//   We'll have tokens for HS 25 _ 09827 : 2 : that are entirely <MATCH>
//   after the initial token.  These 7 tokens could be one ALPHA instead
//   of 7 distinct tokens, with 1 MATCH instead of 7.  This is both a speed
//   improvement for decoding as well as a space saving (fewer token-blocks
//   and associated overhead).
//
// - XOR.  Like ALPHA, but used when previous symbol is ALPHA or XOR
//   and string lengths match.  Useful when names are similar, eg:
//   the sequence in 07.names:
//
//   @VP2-06:112:H7LNDMCVY:1:1105:26919:1172 1:N:0:ATTCAGAA+AGGAGAAG
//   @VP2-06:112:H7LNDMCVY:1:1105:27100:1172 1:N:0:ATTCAGAA+AGGCGAAG
//   @VP2-06:112:H7LNDMCVY:1:1105:27172:1172 1:N:0:ATTCAGAA+AGGCTAAG



#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include "io_lib/pooled_alloc.h"
#include "io_lib/arith_dynamic.h"

// 128 is insufficient for SAM names (max 256 bytes) as
// we may alternate a0a0a0a0a0 etc.  However if we fail,
// we just give up and switch to another codec, so this
// isn't a serious limit.  Maybe up to 256 to permit all
// SAM names?
#define MAX_TOKENS 128
#define MAX_TBLOCKS (MAX_TOKENS<<4)

// Number of names per block
#define MAX_NAMES 1000000

enum name_type {N_ERR = -1, N_TYPE = 0, N_ALPHA, N_CHAR, N_DZLEN, N_DIGITS0, N_DUP, N_DIFF, 
		N_DIGITS, N_D1, N_D2, N_D3, N_DDELTA, N_DDELTA0, N_MATCH, N_END,N_ALL};

char *types[]={"TYPE", "ALPHA", "CHAR", "DZLEN", "DIG0", "DUP", "DIFF",
	       "DIGITS", "", "", "", "DDELTA", "DDELTA0", "MATCH", "END"};

typedef struct trie trie_t;

typedef struct {
    char *last_name;
    int last_ntok;
    enum name_type last_token_type[MAX_TOKENS];
    int last_token_int[MAX_TOKENS];
    int last_token_str[MAX_TOKENS];
    //int last_token_delta[MAX_TOKENS];
} last_context;

typedef struct {
    uint8_t *buf;
    size_t buf_a, buf_l; // alloc and used length.
    int tnum, ttype;
    int dup_from;
} descriptor;

typedef struct {
    last_context *lc;

    // For finding entire line dups
    int counter;

    // Trie used in encoder only
    trie_t *t_head;
    pool_alloc_t *pool;

    // token blocks
    descriptor desc[MAX_TBLOCKS];

    // summary stats per token
    int token_dcount[MAX_TOKENS];
    int token_icount[MAX_TOKENS];
    //int token_zcount[MAX_TOKENS];
} name_context;

name_context *create_context(int max_names) {
    if (max_names <= 0)
	return NULL;

    name_context *ctx = malloc(sizeof(*ctx) + ++max_names*sizeof(*ctx->lc));
    if (!ctx) return NULL;

    ctx->counter = 0;
    ctx->t_head = NULL;

    ctx->lc = (last_context *)(((char *)ctx) + sizeof(*ctx));
    ctx->pool = NULL;

    memset(&ctx->desc[0], 0, MAX_TBLOCKS * sizeof(ctx->desc[0]));
    memset(&ctx->token_dcount[0], 0, MAX_TOKENS * sizeof(int));
    memset(&ctx->token_icount[0], 0, MAX_TOKENS * sizeof(int));
    //memset(&ctx->token_zcount[0], 0, MAX_TOKENS * sizeof(int));

    return ctx;
}

void free_trie(trie_t *t);
void free_context(name_context *ctx) {
    if (!ctx)
	return;

    if (ctx->t_head)
	free(ctx->t_head);
    if (ctx->pool)
	pool_destroy(ctx->pool);

    free(ctx);
}

//-----------------------------------------------------------------------------
// Fast unsigned integer printing code.
// Returns number of bytes written.
static int append_uint32_fixed(char *cp, uint32_t i, uint8_t l) {
    switch (l) {
    case 9:*cp++ = i / 100000000 + '0', i %= 100000000;
    case 8:*cp++ = i / 10000000  + '0', i %= 10000000;
    case 7:*cp++ = i / 1000000   + '0', i %= 1000000;
    case 6:*cp++ = i / 100000    + '0', i %= 100000;
    case 5:*cp++ = i / 10000     + '0', i %= 10000;
    case 4:*cp++ = i / 1000      + '0', i %= 1000;
    case 3:*cp++ = i / 100       + '0', i %= 100;
    case 2:*cp++ = i / 10        + '0', i %= 10;
    case 1:*cp++ = i             + '0';
    case 0:break;
    }
    return l;
}

static int append_uint32_var(char *cp, uint32_t i) {
    char *op = cp;
    uint32_t j;

    //if (i < 10)         goto b0;
    if (i < 100)        goto b1;
    //if (i < 1000)       goto b2;
    if (i < 10000)      goto b3;
    //if (i < 100000)     goto b4;
    if (i < 1000000)    goto b5;
    //if (i < 10000000)   goto b6;
    if (i < 100000000)  goto b7;

    if ((j = i / 1000000000)) {*cp++ = j + '0'; i -= j*1000000000; goto x8;}
    if ((j = i / 100000000))  {*cp++ = j + '0'; i -= j*100000000;  goto x7;}
 b7:if ((j = i / 10000000))   {*cp++ = j + '0'; i -= j*10000000;   goto x6;}
    if ((j = i / 1000000))    {*cp++ = j + '0', i -= j*1000000;    goto x5;}
 b5:if ((j = i / 100000))     {*cp++ = j + '0', i -= j*100000;     goto x4;}
    if ((j = i / 10000))      {*cp++ = j + '0', i -= j*10000;      goto x3;}
 b3:if ((j = i / 1000))       {*cp++ = j + '0', i -= j*1000;       goto x2;}
    if ((j = i / 100))        {*cp++ = j + '0', i -= j*100;        goto x1;}
 b1:if ((j = i / 10))         {*cp++ = j + '0', i -= j*10;         goto x0;}
    if (i)                     *cp++ = i + '0';
    return cp-op;

 x8:*cp++ = i / 100000000 + '0', i %= 100000000;
 x7:*cp++ = i / 10000000  + '0', i %= 10000000;
 x6:*cp++ = i / 1000000   + '0', i %= 1000000;
 x5:*cp++ = i / 100000    + '0', i %= 100000;
 x4:*cp++ = i / 10000     + '0', i %= 10000;
 x3:*cp++ = i / 1000      + '0', i %= 1000;
 x2:*cp++ = i / 100       + '0', i %= 100;
 x1:*cp++ = i / 10        + '0', i %= 10;
 x0:*cp++ = i             + '0';

    return cp-op;
}

//-----------------------------------------------------------------------------
// Simple variable sized unsigned integers
static int i7put(uint8_t *cp, uint64_t i) {
    uint8_t *op = cp;
    int s = 0;
    uint64_t o = i;

    do {
	s += 7;
	o >>= 7;
    } while (o);

    do {
	s -= 7;
	*cp++ = ((i>>s)&0x7f) + (s?128:0);
    } while (s);

    return cp-op;
}

static int i7get(uint8_t *cp, uint64_t *i) {
    uint8_t *op = cp, c;
    uint64_t j = 0;

    do {
	c = *cp++;
	j = (j<<7) | (c & 0x7f);
    } while (c & 0x80);

    *i = j;
    return cp-op;
}

//-----------------------------------------------------------------------------
// Example descriptor encoding and IO.
//
// Here we just append to a buffer so we can dump out the results.
// These could then be passed through a static entropy encoder that
// encodes the entire buffer.
//
// Alternatively an adaptive entropy encoder could be place inline
// here to encode as it goes using additional knowledge from the
// supplied context.

// Ensure room for sz more bytes.
static int descriptor_grow(descriptor *fd, uint32_t sz) {
    while (fd->buf_l + sz > fd->buf_a) {
	size_t buf_a = fd->buf_a ? fd->buf_a*2 : 65536;
	unsigned char *buf = realloc(fd->buf, buf_a);
	if (!buf)
	    return -1;
	fd->buf = buf;
	fd->buf_a = buf_a;
    }

    return 0;
}

static int encode_token_type(name_context *ctx, int ntok,
			     enum name_type type) {
    int id = ntok<<4;

    if (descriptor_grow(&ctx->desc[id], 1) < 0) return -1;

    ctx->desc[id].buf[ctx->desc[id].buf_l++] = type;

    return 0;
}

static int encode_token_match(name_context *ctx, int ntok) {
    return encode_token_type(ctx, ntok, N_MATCH);
}

static int encode_token_end(name_context *ctx, int ntok) {
    return encode_token_type(ctx, ntok, N_END);
}

static enum name_type decode_token_type(name_context *ctx, int ntok) {
    int id = ntok<<4;
    if (ctx->desc[id].buf_l >= ctx->desc[id].buf_a) return -1;
    return ctx->desc[id].buf[ctx->desc[id].buf_l++];
}

// int stored as 32-bit quantities
static int encode_token_int(name_context *ctx, int ntok,
			    enum name_type type, uint32_t val) {
    int id = (ntok<<4) | type;

    if (encode_token_type(ctx, ntok, type) < 0) return -1;
    if (descriptor_grow(&ctx->desc[id], 4) < 0)	return -1;

    // Assumes little endian and unalign access OK.
    *(uint32_t *)(ctx->desc[id].buf + ctx->desc[id].buf_l) = val;
    ctx->desc[id].buf_l += 4;

    return 0;
}

// Return 0 on success, -1 on failure;
static int decode_token_int(name_context *ctx, int ntok,
			    enum name_type type, uint32_t *val) {
    int id = (ntok<<4) | type;

    if (ctx->desc[id].buf_l + 4 > ctx->desc[id].buf_a)
	return -1;

    // Assumes little endian and unalign access OK.
    *val = *(uint32_t *)(ctx->desc[id].buf + ctx->desc[id].buf_l);
    ctx->desc[id].buf_l += 4;

    return 0;
}

// 8 bit integer quantity
static int encode_token_int1(name_context *ctx, int ntok,
			     enum name_type type, uint32_t val) {
    int id = (ntok<<4) | type;

    if (encode_token_type(ctx, ntok, type) < 0) return -1;
    if (descriptor_grow(&ctx->desc[id], 1) < 0)	return -1;

    ctx->desc[id].buf[ctx->desc[id].buf_l++] = val;

    return 0;
}

static int encode_token_int1_(name_context *ctx, int ntok,
			      enum name_type type, uint32_t val) {
    int id = (ntok<<4) | type;

    if (descriptor_grow(&ctx->desc[id], 1) < 0)	return -1;

    ctx->desc[id].buf[ctx->desc[id].buf_l++] = val;

    return 0;
}

// Return 0 on success, -1 on failure;
static int decode_token_int1(name_context *ctx, int ntok,
			     enum name_type type, uint32_t *val) {
    int id = (ntok<<4) | type;

    if (ctx->desc[id].buf_l  >= ctx->desc[id].buf_a)
	return -1;
    *val = ctx->desc[id].buf[ctx->desc[id].buf_l++];

    return 0;
}


// Basic C-string style for now.
//
// Maybe XOR with previous string as context?
// This permits partial match to be encoded efficiently.
static int encode_token_alpha(name_context *ctx, int ntok,
			      char *str, int len) {
    int id = (ntok<<4) | N_ALPHA;

    if (encode_token_type(ctx, ntok, N_ALPHA) < 0)  return -1;
    if (descriptor_grow(&ctx->desc[id], len+1) < 0) return -1;
    memcpy(&ctx->desc[id].buf[ctx->desc[id].buf_l], str, len);
    ctx->desc[id].buf[ctx->desc[id].buf_l+len] = 0;
    ctx->desc[id].buf_l += len+1;

    return 0;
}

// FIXME: need limit on string length for security.
// Return length on success, -1 on failure;
static int decode_token_alpha(name_context *ctx, int ntok, char *str, int max_len) {
    int id = (ntok<<4) | N_ALPHA;
    char c;
    int len = 0;
    if (ctx->desc[id].buf_l  >= ctx->desc[id].buf_a)
	return -1;
    do {
	c = ctx->desc[id].buf[ctx->desc[id].buf_l++];
	str[len++] = c;
    } while(c && len < max_len && ctx->desc[id].buf_l < ctx->desc[id].buf_a);

    return len-1;
}

static int encode_token_char(name_context *ctx, int ntok, char c) {
    int id = (ntok<<4) | N_CHAR;

    if (encode_token_type(ctx, ntok, N_CHAR) < 0) return -1;
    if (descriptor_grow(&ctx->desc[id], 1) < 0)    return -1;
    ctx->desc[id].buf[ctx->desc[id].buf_l++] = c;

    return 0;
}

// FIXME: need limit on string length for security
// Return length on success, -1 on failure;
static int decode_token_char(name_context *ctx, int ntok, char *str) {
    int id = (ntok<<4) | N_CHAR;

    if (ctx->desc[id].buf_l  >= ctx->desc[id].buf_a)
	return -1;
    *str = ctx->desc[id].buf[ctx->desc[id].buf_l++];

    return 1;
}


// A duplicated name
static int encode_token_dup(name_context *ctx, uint32_t val) {
    return encode_token_int(ctx, 0, N_DUP, val);
}

// Which read to delta against
static int encode_token_diff(name_context *ctx, uint32_t val) {
    return encode_token_int(ctx, 0, N_DIFF, val);
}


//-----------------------------------------------------------------------------
// Trie implementation for tracking common name prefixes.
typedef struct trie {
    char c;
    int count;
    //struct trie *next[128];
    struct trie *next, *sibling;
    int n; // Nth line
} trie_t;

//static trie_t *t_head = NULL;

void free_trie(trie_t *t) {
    trie_t *x, *n;
    for (x = t->next; x; x = n) {
	n = x->sibling;
	free_trie(x);
    }
    free(t);
}

int build_trie(name_context *ctx, char *data, size_t len, int n) {
    int nlines = 0;
    size_t i;
    trie_t *t;

    if (!ctx->t_head) {
	ctx->t_head = calloc(1, sizeof(*ctx->t_head));
	if (!ctx->t_head)
	    return -1;
    }

    // Build our trie, also counting input lines
    for (nlines = i = 0; i < len; i++, nlines++) {
	t = ctx->t_head;
	t->count++;
	while (i < len && data[i] > '\n') {
	    unsigned char c = data[i++];
	    if (c & 0x80)
		//fprintf(stderr, "8-bit ASCII is unsupported\n");
		abort();
	    c &= 127;


	    trie_t *x = t->next, *l = NULL;
	    while (x && x->c != c) {
		l = x; x = x->sibling;
	    }
	    if (!x) {
		if (!ctx->pool)
		    ctx->pool = pool_create(sizeof(trie_t));
		if (!(x = (trie_t *)pool_alloc(ctx->pool)))
		    return -1;
		memset(x, 0, sizeof(*x));
		if (!l)
		    x = t->next    = x;
		else
		    x = l->sibling = x;
		x->n = n;
		x->c = c;
	    }
	    t = x;
	    t->c = c;
	    t->count++;
	}
    }

    return 0;
}

#if 0
void dump_trie(trie_t *t, int depth) {
    if (depth == 0) {
	printf("graph x_%p {\n    splines = ortho\n    ranksep=2\n", t);
	printf("    p_%p [label=\"\"];\n", t);
	dump_trie(t, 1);
	printf("}\n");
    } else {
	int j, k, count;//, cj;
	char label[100], *cp;
	trie_t *tp = t;

//    patricia:
//	for (count = j = 0; j < 128; j++)
//	    if (t->next[j])
//		count++, cj=j;
//
//	if (count == 1) {
//	    t = t->next[cj];
//	    *cp++ = cj;
//	    goto patricia;
//	}

	trie_t *x;
	for (x = t->next; x; x = x->sibling) {
	    printf("    p_%p [label=\"%c\"];\n", x, x->c);
	    printf("    p_%p -- p_%p [label=\"%d\", penwidth=\"%f\"];\n", tp, x, x->count, MAX((log(x->count)-3)*2,1));
	    //if (depth <= 11)
		dump_trie(x, depth+1);
	}

#if 0	    
	for (j = 0; j < 128; j++) {
	    trie_t *tn;

	    if (!t->next[j])
		continue;

	    cp = label;
	    tn = t->next[j];
	    *cp++ = j;
//	patricia:

	    for (count = k = 0; k < 128; k++)
		if (tn->next[k])
		    count++;//, cj=k;

//	    if (count == 1) {
//		tn = tn->next[cj];
//		*cp++ = cj;
//		goto patricia;
//	    }
	    *cp++ = 0;

	    printf("    p_%p [label=\"%s\"];\n", tn, label);
	    printf("    p_%p -- p_%p [label=\"%d\", penwidth=\"%f\"];\n", tp, tn, tn->count, MAX((log(tn->count)-3)*2,1));
	    if (depth <= 11)
		dump_trie(tn, depth+1);
	}
#endif
    }
}
#endif

int search_trie(name_context *ctx, char *data, size_t len, int n, int *exact, int *is_fixed, int *fixed_len) {
    int nlines = 0;
    size_t i;
    trie_t *t;
    int from = -1, p3 = -1;

    // Horrid hack for the encoder only.
    // We optimise per known name format here.
    int prefix_len;
    char *d = *data == '@' ? data+1 : data;
    int l   = *data == '@' ? len-1  : len;
    int f = (*data == '>') ? 1 : 0;
    if (l > 70 && d[f+0] == 'm' && d[7] == '_' && d[f+14] == '_' && d[f+61] == '/') {
	prefix_len = 60; // PacBio
	*is_fixed = 0;
    } else if (l == 17 && d[f+5] == ':' && d[f+11] == ':') {
	prefix_len = 7;  // IonTorrent
	*fixed_len = 7;
	*is_fixed = 1;
    } else if (l > 37 && d[f+8] == '-' && d[f+13] == '-' && d[f+18] == '-' && d[f+23] == '-' &&
	       ((d[f+0] >= '0' && d[f+0] <='9') || (d[f+0] >= 'a' && d[f+0] <= 'f')) &&
	       ((d[f+35] >= '0' && d[f+35] <='9') || (d[f+35] >= 'a' && d[f+35] <= 'f'))) {
	// ONT: f33d30d5-6eb8-4115-8f46-154c2620a5da_Basecall_1D_template...
	prefix_len = 37;
	*fixed_len = 37;
	*is_fixed = 1;
    } else {
	// Anything else we give up on the trie method, but we still want to search
	// for exact matches;
	prefix_len = INT_MAX;
	*is_fixed = 0;
    }
    //prefix_len = INT_MAX;

    if (!ctx->t_head) {
	ctx->t_head = calloc(1, sizeof(*ctx->t_head));
	if (!ctx->t_head)
	    return -1;
    }

    // Find an item in the trie
    for (nlines = i = 0; i < len; i++, nlines++) {
	t = ctx->t_head;
	while (i < len && data[i] > '\n') {
	    unsigned char c = data[i++];
	    if (c & 0x80)
		//fprintf(stderr, "8-bit ASCII is unsupported\n");
		abort();
	    c &= 127;

	    trie_t *x = t->next;
	    while (x && x->c != c)
		x = x->sibling;
	    t = x;

//	    t = t->next[c];

//	    if (!t)
//		return -1;

	    from = t->n;
	    if (i == prefix_len) p3 = t->n;
	    //if (t->count >= .0035*ctx->t_head->count && t->n != n) p3 = t->n; // pacbio
	    //if (i == 60) p3 = t->n; // pacbio
	    //if (i == 7) p3 = t->n; // iontorrent
	    t->n = n;
	}
    }

    //printf("Looked for %d, found %d, prefix %d\n", n, from, p3);

    *exact = (n != from);
    return *exact ? from : p3;
}


//-----------------------------------------------------------------------------
// Name encoder

/*
 * Tokenises a read name using ctx as context as the previous
 * tokenisation.
 *
 * Parsed elements are then emitted for encoding by calling the
 * encode_token() function with the context, token number (Nth token
 * in line), token type and token value.
 *
 * Returns 0 on success;
 *        -1 on failure.
 */
static int encode_name(name_context *ctx, char *name, int len, int mode) {
    int i, is_fixed, fixed_len;

    int exact;
    int cnum = ctx->counter++;
    int pnum = search_trie(ctx, name, len, cnum, &exact, &is_fixed, &fixed_len);
    if (pnum < 0) pnum = cnum ? cnum-1 : 0;
    //pnum = pnum & (MAX_NAMES-1);
    //cnum = cnum & (MAX_NAMES-1);
    //if (pnum == cnum) {pnum = cnum ? cnum-1 : 0;}
#ifdef ENC_DEBUG
    fprintf(stderr, "%d: pnum=%d (%d), exact=%d\n%s\n%s\n",
	    ctx->counter, pnum, cnum-pnum, exact, ctx->lc[pnum].last_name, name);
#endif

    // Return DUP or DIFF switch, plus the distance.
    if (exact && len == strlen(ctx->lc[pnum].last_name)) {
	encode_token_dup(ctx, cnum-pnum);
	ctx->lc[cnum].last_name = name;
	ctx->lc[cnum].last_ntok = ctx->lc[pnum].last_ntok;
	// FIXME: optimise this
	int nc = ctx->lc[cnum].last_ntok ? ctx->lc[cnum].last_ntok : MAX_TOKENS;
	memcpy(ctx->lc[cnum].last_token_type, ctx->lc[pnum].last_token_type, nc * sizeof(int));
	memcpy(ctx->lc[cnum].last_token_int , ctx->lc[pnum].last_token_int , nc * sizeof(int));
	memcpy(ctx->lc[cnum].last_token_str , ctx->lc[pnum].last_token_str , nc * sizeof(int));
	return 0;
    }

    encode_token_diff(ctx, cnum-pnum);

    int ntok = 1;
    i = 0;
    if (is_fixed) {
	if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok && ctx->lc[pnum].last_token_type[ntok] == N_ALPHA) {
	    if (ctx->lc[pnum].last_token_int[ntok] == fixed_len && memcmp(name, ctx->lc[pnum].last_name, fixed_len) == 0) {
		encode_token_match(ctx, ntok);
	    } else {
		encode_token_alpha(ctx, ntok, name, fixed_len);
	    }
	} else {
	    encode_token_alpha(ctx, ntok, name, fixed_len);
	}
	ctx->lc[cnum].last_token_int[ntok] = fixed_len;
	ctx->lc[cnum].last_token_str[ntok] = 0;
	ctx->lc[cnum].last_token_type[ntok++] = N_ALPHA;
	i = fixed_len;
    }

    for (; i < len; i++) {
	/* Determine data type of this segment */
	if (isalpha(name[i])) {
	    int s = i+1;
//	    int S = i+1;

//	    // FIXME: try which of these is best.  alnum is good sometimes.
//	    while (s < len && isalpha(name[s]))
	    while (s < len && (isalpha(name[s]) || ispunct(name[s])))
//	    while (s < len && name[s] != ':')
//	    while (s < len && !isdigit(name[s]) && name[s] != ':')
		s++;

//	    if (!is_fixed) {
//		while (S < len && isalnum(name[S]))
//		    S++;
//		if (s < S)
//		    s = S;
//	    }

	    // Single byte strings are better encoded as chars.
	    if (s-i == 1) goto n_char;

	    if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok && ctx->lc[pnum].last_token_type[ntok] == N_ALPHA) {
		if (s-i == ctx->lc[pnum].last_token_int[ntok] &&
		    memcmp(&name[i], 
			   &ctx->lc[pnum].last_name[ctx->lc[pnum].last_token_str[ntok]],
			   s-i) == 0) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (alpha-mat, %.*s)\n", N_MATCH, s-i, &name[i]);
#endif
		    if (encode_token_match(ctx, ntok) < 0) return -1;
		} else {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (alpha, %.*s / %.*s)\n", N_ALPHA,
		    	    s-i, &ctx->lc[pnum].last_name[ctx->lc[pnum].last_token_str[ntok]], s-i, &name[i]);
#endif
		    // same token/length, but mismatches
		    if (encode_token_alpha(ctx, ntok, &name[i], s-i) < 0) return -1;
		}
	    } else {
#ifdef ENC_DEBUG
		fprintf(stderr, "Tok %d (new alpha, %.*s)\n", N_ALPHA, s-i, &name[i]);
#endif
		if (encode_token_alpha(ctx, ntok, &name[i], s-i) < 0) return -1;
	    }

	    ctx->lc[cnum].last_token_int[ntok] = s-i;
	    ctx->lc[cnum].last_token_str[ntok] = i;
	    ctx->lc[cnum].last_token_type[ntok] = N_ALPHA;

	    i = s-1;
	} else if (name[i] == '0') digits0: {
	    // Digits starting with zero; encode length + value
	    uint32_t s = i;
	    uint32_t v = 0;
	    int d = 0;

	    while (s < len && isdigit(name[s]) && s-i < 9) {
		v = v*10 + name[s] - '0';
		//putchar(name[s]);
		s++;
	    }

	    // TODO: optimise choice over whether to switch from DIGITS to DELTA
	    // regularly vs all DIGITS, also MATCH vs DELTA 0.
	    if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok && ctx->lc[pnum].last_token_type[ntok] == N_DIGITS0) {
		d = v - ctx->lc[pnum].last_token_int[ntok];
		if (d == 0 && ctx->lc[pnum].last_token_str[ntok] == s-i) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig-mat, %d)\n", N_MATCH, v);
#endif
		    if (encode_token_match(ctx, ntok) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=0;
		} else if (mode == 1 && d < 256 && d >= 0 && ctx->lc[pnum].last_token_str[ntok] == s-i) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig-delta, %d / %d)\n", N_DDELTA, ctx->lc[pnum].last_token_int[ntok], v);
#endif
		    //if (encode_token_int1_(ctx, ntok, N_DZLEN, s-i) < 0) return -1;
		    if (encode_token_int1(ctx, ntok, N_DDELTA0, d) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=1;
		} else {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig, %d / %d)\n", N_DIGITS, ctx->lc[pnum].last_token_int[ntok], v);
#endif
		    if (encode_token_int1_(ctx, ntok, N_DZLEN, s-i) < 0) return -1;
		    if (encode_token_int(ctx, ntok, N_DIGITS0, v) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=0;
		}
	    } else {
#ifdef ENC_DEBUG
		fprintf(stderr, "Tok %d (new dig, %d)\n", N_DIGITS, v);
#endif
		if (encode_token_int1_(ctx, ntok, N_DZLEN, s-i) < 0) return -1;
		if (encode_token_int(ctx, ntok, N_DIGITS0, v) < 0) return -1;
		//ctx->lc[pnum].last_token_delta[ntok]=0;
	    }

	    ctx->lc[cnum].last_token_str[ntok] = s-i; // length
	    ctx->lc[cnum].last_token_int[ntok] = v;
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS0;

	    i = s-1;
	} else if (isdigit(name[i])) {
	    // digits starting 1-9; encode value
	    uint32_t s = i;
	    uint32_t v = 0;
	    int d = 0;

	    while (s < len && isdigit(name[s]) && s-i < 9) {
		v = v*10 + name[s] - '0';
		//putchar(name[s]);
		s++;
	    }

	    // dataset/10/K562_cytosol_LID8465_TopHat_v2.names
	    // col 4 is Illumina lane - we don't want match & delta in there
	    // as it has multiple lanes (so not ALL match) and delta is just
	    // random chance, increasing entropy instead.
//	    if (ntok == 4  || ntok == 8 || ntok == 10) {
//		encode_token_int(ctx, ntok, N_DIGITS, v);
//	    } else {

	    // If the last token was DIGITS0 and we are the same length, then encode
	    // using that method instead as it seems likely the entire column is fixed
	    // width, sometimes with leading zeros.
	    if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok &&
		ctx->lc[pnum].last_token_type[ntok] == N_DIGITS0 &&
		ctx->lc[pnum].last_token_str[ntok] == s-i)
		goto digits0;
	    
	    // TODO: optimise choice over whether to switch from DIGITS to DELTA
	    // regularly vs all DIGITS, also MATCH vs DELTA 0.
	    if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok && ctx->lc[pnum].last_token_type[ntok] == N_DIGITS) {
		d = v - ctx->lc[pnum].last_token_int[ntok];
		if (d == 0) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig-mat, %d)\n", N_MATCH, v);
#endif
		    if (encode_token_match(ctx, ntok) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=0;
		    //ctx->token_zcount[ntok]++;
		} else if (mode == 1 && d < 256 && d >= 0
			   //&& (10+ctx->token_dcount[ntok]) > (ctx->token_icount[ntok]+ctx->token_zcount[ntok])
			   && (5+ctx->token_dcount[ntok]) > ctx->token_icount[ntok]
			   ) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig-delta, %d / %d)\n", N_DDELTA, ctx->lc[pnum].last_token_int[ntok], v);
#endif
		    if (encode_token_int1(ctx, ntok, N_DDELTA, d) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=1;
		    ctx->token_dcount[ntok]++;
		} else {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (dig, %d / %d)\n", N_DIGITS, ctx->lc[pnum].last_token_int[ntok], v);
#endif
		    if (encode_token_int(ctx, ntok, N_DIGITS, v) < 0) return -1;
		    //ctx->lc[pnum].last_token_delta[ntok]=0;
		    ctx->token_icount[ntok]++;
		}
	    } else {
#ifdef ENC_DEBUG
		fprintf(stderr, "Tok %d (new dig, %d)\n", N_DIGITS, v);
#endif
		if (encode_token_int(ctx, ntok, N_DIGITS, v) < 0) return -1;
		//ctx->lc[pnum].last_token_delta[ntok]=0;
	    }
//	    }

	    ctx->lc[cnum].last_token_int[ntok] = v;
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS;

	    i = s-1;
	} else {
	n_char:
	    //if (!isalpha(name[i])) putchar(name[i]);
	    if (pnum < cnum && ntok < ctx->lc[pnum].last_ntok && ctx->lc[pnum].last_token_type[ntok] == N_CHAR) {
		if (name[i] == ctx->lc[pnum].last_token_int[ntok]) {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (chr-mat, %c)\n", N_MATCH, name[i]);
#endif
		    if (encode_token_match(ctx, ntok) < 0) return -1;
		} else {
#ifdef ENC_DEBUG
		    fprintf(stderr, "Tok %d (chr, %c / %c)\n", N_CHAR, ctx->lc[pnum].last_token_int[ntok], name[i]);
#endif
		    if (encode_token_char(ctx, ntok, name[i]) < 0) return -1;
		}
	    } else {
#ifdef ENC_DEBUG
		fprintf(stderr, "Tok %d (new chr, %c)\n", N_CHAR, name[i]);
#endif
		if (encode_token_char(ctx, ntok, name[i]) < 0) return -1;
	    }

	    ctx->lc[cnum].last_token_int[ntok] = name[i];
	    ctx->lc[cnum].last_token_type[ntok] = N_CHAR;
	}

	ntok++;
	//putchar(' ');
    }

#ifdef ENC_DEBUG
    fprintf(stderr, "Tok %d (end)\n", N_END);
#endif
    if (encode_token_end(ctx, ntok) < 0) return -1;

    //printf("Encoded %.*s with %d tokens\n", len, name, ntok);
    
    ctx->lc[cnum].last_name = name;
    ctx->lc[cnum].last_ntok = ntok;

    return 0;
}

//-----------------------------------------------------------------------------
// Name decoder

static int decode_name(name_context *ctx, char *name, int name_len) {
    int t0 = decode_token_type(ctx, 0);
    uint32_t dist;
    int pnum, cnum = ctx->counter++;

    if (t0 < 0)
	return 0;

    if (decode_token_int(ctx, 0, t0, &dist) < 0 || dist > cnum)
	return -1;
    if ((pnum = cnum - dist) < 0) pnum = 0;

    //fprintf(stderr, "t0=%d, dist=%d, pnum=%d, cnum=%d\n", t0, dist, pnum, cnum);

    if (t0 == N_DUP) {
	if (strlen(ctx->lc[pnum].last_name) +1 >= name_len) return -1;
	strcpy(name, ctx->lc[pnum].last_name);
	// FIXME: optimise this
	ctx->lc[cnum].last_name = name;
	ctx->lc[cnum].last_ntok = ctx->lc[pnum].last_ntok;
	int nc = ctx->lc[cnum].last_ntok ? ctx->lc[cnum].last_ntok : MAX_TOKENS;
	memcpy(ctx->lc[cnum].last_token_type, ctx->lc[pnum].last_token_type, nc * sizeof(int));
	memcpy(ctx->lc[cnum].last_token_int , ctx->lc[pnum].last_token_int , nc * sizeof(int));
	memcpy(ctx->lc[cnum].last_token_str , ctx->lc[pnum].last_token_str , nc * sizeof(int));

	return strlen(name)+1;
    }

    *name = 0;
    int ntok, len = 0, len2;

    for (ntok = 1; ntok < MAX_TOKENS; ntok++) {
	uint32_t v, vl;
	enum name_type tok;
	tok = decode_token_type(ctx, ntok);
	//fprintf(stderr, "Tok %d = %d\n", ntok, tok);

	switch (tok) {
	case N_CHAR:
	    if (len+1 >= name_len) return -1;
	    if (decode_token_char(ctx, ntok, &name[len]) < 0) return -1;
	    //fprintf(stderr, "Tok %d CHAR %c\n", ntok, name[len]);
	    ctx->lc[cnum].last_token_type[ntok] = N_CHAR;
	    ctx->lc[cnum].last_token_int [ntok] = name[len++];
	    break;

	case N_ALPHA:
	    len2 = decode_token_alpha(ctx, ntok, &name[len], name_len - len);
	    //fprintf(stderr, "Tok %d ALPHA %.*s\n", ntok, len2, &name[len]);
	    ctx->lc[cnum].last_token_type[ntok] = N_ALPHA;
	    ctx->lc[cnum].last_token_str [ntok] = len;
	    ctx->lc[cnum].last_token_int [ntok] = len2;
	    len += len2;
	    break;

	case N_DIGITS0: // [0-9]*
	    if (decode_token_int1(ctx, ntok, N_DZLEN, &vl) < 0) return -1;
	    if (decode_token_int(ctx, ntok, N_DIGITS0, &v) < 0) return -1;
	    if (len+20+vl >= name_len) return -1;
	    len += append_uint32_fixed(&name[len], v, vl);
	    //fprintf(stderr, "Tok %d DIGITS0 %0*d\n", ntok, vl, v);
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS0;
	    ctx->lc[cnum].last_token_int [ntok] = v;
	    ctx->lc[cnum].last_token_str [ntok] = vl;
	    break;

	case N_DDELTA0:
	    if (decode_token_int1(ctx, ntok, N_DDELTA0, &v) < 0) return -1;
	    v += ctx->lc[pnum].last_token_int[ntok];
	    if (len+ctx->lc[pnum].last_token_str[ntok]+1 >= name_len) return -1;
	    len += append_uint32_fixed(&name[len], v, ctx->lc[pnum].last_token_str[ntok]);
	    //fprintf(stderr, "Tok %d DELTA0 %0*d\n", ntok, ctx->lc[pnum].last_token_str[ntok], v);
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS0;
	    ctx->lc[cnum].last_token_int [ntok] = v;
	    ctx->lc[cnum].last_token_str [ntok] = ctx->lc[pnum].last_token_str[ntok];
	    break;

	case N_DIGITS: // [1-9][0-9]*
	    if (decode_token_int(ctx, ntok, N_DIGITS, &v) < 0) return -1;
	    if (len+20 >= name_len) return -1;
	    len += append_uint32_var(&name[len], v);
	    //fprintf(stderr, "Tok %d DIGITS %d\n", ntok, v);
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS;
	    ctx->lc[cnum].last_token_int [ntok] = v;
	    break;

	case N_DDELTA:
	    if (decode_token_int1(ctx, ntok, N_DDELTA, &v) < 0) return -1;
	    v += ctx->lc[pnum].last_token_int[ntok];
	    if (len+20 >= name_len) return -1;
	    len += append_uint32_var(&name[len], v);
	    //fprintf(stderr, "Tok %d DELTA %d\n", ntok, v);
	    ctx->lc[cnum].last_token_type[ntok] = N_DIGITS;
	    ctx->lc[cnum].last_token_int [ntok] = v;
	    break;

	case N_MATCH:
	    switch (ctx->lc[pnum].last_token_type[ntok]) {
	    case N_CHAR:
		if (len+1 >= name_len) return -1;
		name[len++] = ctx->lc[pnum].last_token_int[ntok];
		//fprintf(stderr, "Tok %d MATCH CHAR %c\n", ntok, ctx->lc[pnum].last_token_int[ntok]);
		ctx->lc[cnum].last_token_type[ntok] = N_CHAR;
		ctx->lc[cnum].last_token_int [ntok] = ctx->lc[pnum].last_token_int[ntok];
		break;

	    case N_ALPHA:
		if (ctx->lc[pnum].last_token_int[ntok] < 0 ||
		    len+ctx->lc[pnum].last_token_int[ntok] >= name_len) return -1;
		memcpy(&name[len],
		       &ctx->lc[pnum].last_name[ctx->lc[pnum].last_token_str[ntok]],
		       ctx->lc[pnum].last_token_int[ntok]);
		//fprintf(stderr, "Tok %d MATCH ALPHA %.*s\n", ntok, ctx->lc[pnum].last_token_int[ntok], &name[len]);
		ctx->lc[cnum].last_token_type[ntok] = N_ALPHA;
		ctx->lc[cnum].last_token_str [ntok] = len;
		ctx->lc[cnum].last_token_int [ntok] = ctx->lc[pnum].last_token_int[ntok];
		len += ctx->lc[pnum].last_token_int[ntok];
		break;

	    case N_DIGITS:
		if (len+20 >= name_len) return -1;
		len += append_uint32_var(&name[len], ctx->lc[pnum].last_token_int[ntok]);
		//fprintf(stderr, "Tok %d MATCH DIGITS %d\n", ntok, ctx->lc[pnum].last_token_int[ntok]);
		ctx->lc[cnum].last_token_type[ntok] = N_DIGITS;
		ctx->lc[cnum].last_token_int [ntok] = ctx->lc[pnum].last_token_int[ntok];
		break;

	    case N_DIGITS0:
		if (len+ctx->lc[pnum].last_token_str[ntok] >= name_len) return -1;
		len += append_uint32_fixed(&name[len], ctx->lc[pnum].last_token_int[ntok], ctx->lc[pnum].last_token_str[ntok]);
		//fprintf(stderr, "Tok %d MATCH DIGITS %0*d\n", ntok, ctx->lc[pnum].last_token_str[ntok], ctx->lc[pnum].last_token_int[ntok]);
		ctx->lc[cnum].last_token_type[ntok] = N_DIGITS0;
		ctx->lc[cnum].last_token_int [ntok] = ctx->lc[pnum].last_token_int[ntok];
		ctx->lc[cnum].last_token_str [ntok] = ctx->lc[pnum].last_token_str[ntok];
		break;

	    default:
		return -1;
	    }
	    break;

	default: // an elided N_END
	case N_END:
	    if (len+1 >= name_len) return -1;
	    name[len++] = 0;
	    ctx->lc[cnum].last_token_type[ntok] = N_END;

	    ctx->lc[cnum].last_name = name;
	    ctx->lc[cnum].last_ntok = ntok;
	    
	    return len;
	}
    }


    return -1;
}


//-----------------------------------------------------------------------------
// arith adaptive codec
static int arith_encode(uint8_t *in, uint64_t in_len, uint8_t *out, uint64_t *out_len, int method) {
    unsigned int olen = *out_len-6, nb;
    if (arith_compress_to(in, in_len, out+6, &olen, method) == NULL)
	return -1;

    nb = i7put(out, olen);
    memmove(out+nb, out+6, olen);
    *out_len = olen+nb;

    return 0;
}

// Returns number of bytes read from 'in' on success,
//        -1 on failure.
static int64_t arith_decode(uint8_t *in, uint64_t in_len, uint8_t *out, uint64_t *out_len) {
    unsigned int olen = *out_len;

    uint64_t clen;
    int nb = i7get(in, &clen);
    //fprintf(stderr, "Arith decode %x\n", in[nb]);
    if (arith_uncompress_to(in+nb, in_len-nb, out, &olen) == NULL)
	return -1;
    //fprintf(stderr, "    Stored clen=%d\n", (int)clen);
    return clen+nb;
}

static int compress(uint8_t *in, uint64_t in_len, uint8_t *out, uint64_t *out_len) {
    uint64_t best_sz = UINT64_MAX;
    int best = 0;
    uint64_t olen = *out_len;

    //fprintf(stderr, "=== try %d ===\n", (int)in_len);

    if (in_len <= 3) {
	out[0] = in_len;
	memcpy(out+1, in, in_len);
	*out_len = in_len+1;
	return 0;
    }

    int rmethods[] = {0,1,128,129,64,65,192,193, 193+8, 0+4}, m; // slower mode
    //int rmethods[] = {0,128,64,192, 193+8, 0+4}, m; // fast mode
    for (m = 0; m < sizeof(rmethods)/sizeof(*rmethods); m++) {
	*out_len = olen;
	if (arith_encode(in, in_len, out, out_len, rmethods[m]) < 0) return -1;

	if (best_sz > *out_len) {
	    best_sz = *out_len;
	    best = rmethods[m];
	}
    }

    *out_len = olen;
    if (arith_encode(in, in_len, out, out_len, best) < 0) return -1;

    assert(*out_len > 2);

//    uint64_t tmp;
//    fprintf(stderr, "%d -> %d via method %x, %x\n", (int)in_len, (int)best_sz, best, out[i7get(out,&tmp)]);

    return 0;
}

static uint64_t uncompressed_size(uint8_t *in, uint64_t in_len) {
    uint64_t clen, ulen;

    int nb = i7get(in, &clen);
    if (clen <= 3) return clen;

    i7get(in+nb+1, &ulen);

    return ulen;
}

static int uncompress(uint8_t *in, uint64_t in_len, uint8_t *out, uint64_t *out_len) {
    uint64_t clen;
    i7get(in, &clen);

    if (clen <= 3) {
	memcpy(out, in+1, clen);
	*out_len = clen;
	return clen+1;
	
	//*out = in[1];
	//*out_len = 1;
	//return 2;
    }

    return arith_decode(in, in_len, out, out_len);
}

//-----------------------------------------------------------------------------

/*
 * Converts a line or \0 separated block of reading names to a compressed buffer.
 * The code can only encode whole lines and will not attempt a partial line.
 * Use the "last_start_p" return value to identify the partial line start
 * offset, for continuation purposes.
 *
 * Returns a malloced buffer holding compressed data of size *out_len,
 *         or NULL on failure
 */
uint8_t *encode_names(char *blk, int len, int *out_len, int *last_start_p) {
    int last_start = 0, i, j, nreads;
    
    // Count lines
    for (nreads = i = 0; i < len; i++)
	if (blk[i] <= '\n') // \n or \0 separated entries
	    nreads++;

    name_context *ctx = create_context(nreads);
    if (!ctx)
	return NULL;

    // Construct trie
    int ctr = 0;
    for (i = j = 0; i < len; j=++i) {
	while (i < len && blk[i] > '\n')
	    i++;
	if (i >= len)
	    break;

	//blk[i] = '\0';
	last_start = i+1;
	if (build_trie(ctx, &blk[j], i-j, ctr++) < 0) {
	    free_context(ctx);
	    return NULL;
	}
    }
    if (last_start_p)
	*last_start_p = last_start;

    //fprintf(stderr, "Processed %d of %d in block, line %d\n", last_start, len, ctr);

    // Encode name
    for (i = j = 0; i < len; j=++i) {
	while (i < len && blk[i] > '\n')
	    i++;
	if (i >= len)
	    break;

	blk[i] = '\0';
	// try both 0 and 1 and pick best?
	if (encode_name(ctx, &blk[j], i-j, 1) < 0) {
	    free_context(ctx);
	    return NULL;
	}
    }

#if 0
    for (i = 0; i < MAX_TBLOCKS; i++) {
	char fn[1024];
	if (!ctx->desc[i].buf_l) continue;
	sprintf(fn, "_tok.%02d_%02d.%d", i>>4,i&15,i);
	FILE *fp = fopen(fn, "w");
	fwrite(ctx->desc[i].buf, 1, ctx->desc[i].buf_l, fp);
	fclose(fp);
    }
#endif

    //dump_trie(t_head, 0);

    // FIXME: merge descriptors
    //
    // If we see foo7:1 foo7:12 foo7:7 etc then foo: is constant,
    // but it's encoded as alpha<foo>+dig<7>+char<:> instead of alpha<foo7:>.
    // Any time token type 0 is all match beyond the first location we have
    // a candidate for merging in string form.
    //
    // This saves around .1 to 1.3 percent on varying data sets.
    // Cruder hack is dedicated prefix/suffix matching to short-cut this.


    // Drop N_TYPE blocks if they all contain matches bar the first item,
    // as we can regenerate these from the subsequent blocks types during
    // decode.
    for (i = 0; i < MAX_TBLOCKS; i+=16) {
	if (!ctx->desc[i].buf_l) continue;

	int z;
	for (z=1; z<ctx->desc[i].buf_l; z++) {
	    if (ctx->desc[i].buf[z] != N_MATCH)
		break;
	}
	if (z == ctx->desc[i].buf_l) {
	    int k;
	    for (k=1; k<16; k++)
		if (ctx->desc[i+k].buf_l)
		    break;

	    ctx->desc[i].buf_l = 0;
	    free(ctx->desc[i].buf);
	}
    }

    // Serialise descriptors
    uint32_t tot_size = 8;
    int ndesc = 0;
    for (i = 0; i < MAX_TBLOCKS; i++) {
	if (!ctx->desc[i].buf_l) continue;

	ndesc++;

	int tnum = i>>4;
	int ttype = i&15;

	uint64_t out_len = 1.5 * arith_compress_bound(ctx->desc[i].buf_l, 1); // guesswork
	uint8_t *out = malloc(out_len);
	if (!out) {
	    free_context(ctx);
	    return NULL;
	}

	if (compress(ctx->desc[i].buf, ctx->desc[i].buf_l, out, &out_len) < 0) {
	    free_context(ctx);
	    return NULL;
	}

	free(ctx->desc[i].buf);
	ctx->desc[i].buf = out;
	ctx->desc[i].buf_l = out_len;
	ctx->desc[i].tnum = tnum;
	ctx->desc[i].ttype = ttype;

	// Find dups
	int j;
	for (j = 0; j < i; j++) {
	    if (!ctx->desc[j].buf)
		continue;
	    if (ctx->desc[i].buf_l != ctx->desc[j].buf_l || ctx->desc[i].buf_l <= 4)
		continue;
	    if (memcmp(ctx->desc[i].buf, ctx->desc[j].buf, ctx->desc[i].buf_l) == 0)
		break;
	}
	if (j < i) {
	    ctx->desc[i].dup_from = j;
	    tot_size += 4; // flag, dup_from, ttype
	} else {
	    ctx->desc[i].dup_from = 0;
	    tot_size += out_len + 1; // ttype
	}
    }

#if 0
    for (i = 0; i < MAX_TBLOCKS; i++) {
	char fn[1024];
	if (!ctx->desc[i].buf_l && !ctx->desc[i].dup_from) continue;
	sprintf(fn, "_tok.%02d_%02d.%d.comp", i>>4,i&15,i);
	FILE *fp = fopen(fn, "w");
	fwrite(ctx->desc[i].buf, 1, ctx->desc[i].buf_l, fp);
	fclose(fp);
    }
#endif

    // Write
    uint8_t *out = malloc(tot_size+12);
    if (!out) {
	free_context(ctx);
	return NULL;
    }

    uint8_t *cp = out;
    //*out_len = tot_size+4;
    //*(uint32_t *)cp = tot_size;   cp += 4;

    *out_len = tot_size;
    *(uint32_t *)cp = last_start; cp += 4;
    *(uint32_t *)cp = nreads;     cp += 4;
    //write(1, &nreads, 4);
    int last_tnum = -1;
    for (i = 0; i < MAX_TBLOCKS; i++) {
	if (!ctx->desc[i].buf_l) continue;
	uint8_t ttype8 = ctx->desc[i].ttype;
	if (ctx->desc[i].tnum != last_tnum) {
	    ttype8 |= 128;
	    last_tnum = ctx->desc[i].tnum;
	}
	if (ctx->desc[i].dup_from) {
	    //fprintf(stderr, "Dup %d from %d, sz %d\n", i, ctx->desc[i].dup_from, ctx->desc[i].buf_l);
	    //uint8_t x = 255;
	    //write(1, &x, 1);
	    *cp++ = 255;
	    //uint16_t y = ctx->desc[i].dup_from;
	    //write(1, &y, 2);
	    *(uint16_t *)cp = ctx->desc[i].dup_from; cp+= 2;
	    //write(1, &ttype8, 1);
	    *cp++ = ttype8;
	} else {
	    //write(1, &ttype8, 1);
	    //write(1, ctx->desc[i].buf, ctx->desc[i].buf_l);
	    *cp++ = ttype8;
	    memcpy(cp, ctx->desc[i].buf, ctx->desc[i].buf_l);
	    cp += ctx->desc[i].buf_l;
	}
    }

    //assert(cp-out == tot_size);

    for (i = 0; i < MAX_TBLOCKS; i++) {
	if (!ctx->desc[i].buf_l) continue;
	free(ctx->desc[i].buf);
    }

    free_context(ctx);

    return out;
}

/*
 * Decodes a compressed block of read names into \0 separated names.
 * The size of the data returned (malloced) is in *out_len.
 *
 * Returns NULL on failure.
 */
uint8_t *decode_names(uint8_t *in, uint32_t sz, int *out_len) {
    if (sz < 8)
	return NULL;

    int i, o = 8;
    int ulen   = *(uint32_t *)in;
    int nreads = *(uint32_t *)(in+4);
    name_context *ctx = create_context(nreads);
    if (!ctx)
	return NULL;

    // Unpack descriptors
    int tnum = -1;
    while (o < sz) {
	uint8_t ttype = in[o++];
	if (ttype == 255) {
	    if (o+3 >= sz) return NULL;
	    uint16_t j = *(uint16_t *)&in[o];
	    o += 2;
	    ttype = in[o++];
	    //		if (ttype == 0)
	    if (ttype & 128)
		tnum++;

	    if ((ttype & 15) != 0 && (ttype & 128)) {
		ctx->desc[tnum<<4].buf = malloc(nreads);
		if (!ctx->desc[tnum<<4].buf)
		    return NULL;

		ctx->desc[tnum<<4].buf_a = nreads;
		ctx->desc[tnum<<4].buf[0] = ttype&15;
		if ((ttype&15) == N_DZLEN)
		    ctx->desc[tnum<<4].buf[0] = N_DIGITS0;
		memset(&ctx->desc[tnum<<4].buf[1], N_MATCH, nreads-1);
	    }

	    i = (tnum<<4) | (ttype&15);
	    if (j >= i)
		return NULL;

	    ctx->desc[i].buf_l = 0;
	    ctx->desc[i].buf_a = ctx->desc[j].buf_a;
	    ctx->desc[i].buf = malloc(ctx->desc[i].buf_a);
	    if (!ctx->desc[i].buf)
		return NULL;

	    memcpy(ctx->desc[i].buf, ctx->desc[j].buf, ctx->desc[i].buf_a);
	    //fprintf(stderr, "Copy ttype %d, i=%d,j=%d, size %d\n", ttype, i, j, (int)ctx->desc[i].buf_a);
	    continue;
	}

	//if (ttype == 0)
	if (ttype & 128)
	    tnum++;

	if ((ttype & 15) != 0 && (ttype & 128)) {
	    ctx->desc[tnum<<4].buf = malloc(nreads);
	    if (!ctx->desc[tnum<<4].buf) {
		free_context(ctx);
		return NULL;
	    }
	    ctx->desc[tnum<<4].buf_a = nreads;
	    ctx->desc[tnum<<4].buf[0] = ttype&15;
	    if ((ttype&15) == N_DZLEN)
		ctx->desc[tnum<<4].buf[0] = N_DIGITS0;
	    memset(&ctx->desc[tnum<<4].buf[1], N_MATCH, nreads-1);
	}

	//fprintf(stderr, "Read %02x\n", c);

	// Load compressed block
	int64_t clen, ulen = uncompressed_size(&in[o], sz-o);
	if (ulen < 0) {
	    free_context(ctx);
	    return NULL;
	}
	i = (tnum<<4) | (ttype&15);

	if (i >= MAX_TBLOCKS || i < 0)
	    return NULL;

	ctx->desc[i].buf_l = 0;
	ctx->desc[i].buf = malloc(ulen);
	if (!ctx->desc[i].buf) {
	    free_context(ctx);
	    return NULL;
	}

	ctx->desc[i].buf_a = ulen;
	uint64_t usz = ctx->desc[i].buf_a; // convert from size_t for 32-bit sys
	clen = uncompress(&in[o], sz-o, ctx->desc[i].buf, &usz);
	ctx->desc[i].buf_a = usz;
	if (clen < 0) {
	    free(ctx->desc[i].buf);
	    free_context(ctx);
	    return NULL;
	}
	assert(ctx->desc[i].buf_a == ulen);

	//	    fprintf(stderr, "%d: Decode tnum %d type %d clen %d ulen %d via %d\n",
	//		    o, tnum, ttype, (int)clen, (int)ctx->desc[i].buf_a, ctx->desc[i].buf[0]);

	o += clen;

	// Encode tnum 0 type 0 ulen 100000 clen 12530 via 2
	// Encode tnum 0 type 6 ulen 196800 clen 43928 via 3
	// Encode tnum 0 type 7 ulen 203200 clen 17531 via 3
	// Encode tnum 1 type 0 ulen 50800 clen 10 via 1
	// Encode tnum 1 type 1 ulen 3 clen 5 via 0
	// Encode tnum 2 type 0 ulen 50800 clen 10 via 1
	// 	
    }

    int ret;
    ulen += 1024; // for easy coding in decode_name.
    uint8_t *out = malloc(ulen);
    if (!out) {
	free_context(ctx);
	return NULL;
    }

    size_t out_sz = 0;
    while ((ret = decode_name(ctx, (char *)out+out_sz, ulen)) > 0) {
	out_sz += ret;
	ulen -= ret;
    }

    if (ret < 0)
	free(out);

    for (i = 0; i < MAX_TBLOCKS; i++) {
	if (ctx->desc[i].buf) {
	    free(ctx->desc[i].buf);
	    ctx->desc[i].buf = 0;
	}
    }

    free_context(ctx);

    *out_len = out_sz;
    return ret == 0 ? out : NULL;
}



#ifdef TEST_TOKENISER
//-----------------------------------------------------------------------------
// main() implementation for testing

// Large enough for whole file for now.
#ifndef BLK_SIZE
#define BLK_SIZE 1*1024*1024
#endif
static char blk[BLK_SIZE*2]; // temporary fix for decoder, which needs more space

static int encode(int argc, char **argv) {
    FILE *fp;
    int len, i, j;
    name_context *ctx;

    if (argc > 1) {
	fp = fopen(argv[1], "r");
	if (!fp) {
	    perror(argv[1]);
	    return 1;
	}
    } else {
	fp = stdin;
    }

    int blk_offset = 0;
    int blk_num = 0;
    for (;;) {
	int last_start = 0;

	len = fread(blk+blk_offset, 1, BLK_SIZE-blk_offset, fp);
	if (len <= 0)
	    break;
	len += blk_offset;

	int out_len;
	uint8_t *out = encode_names(blk, len, &out_len, &last_start);
	if (write(1, &out_len, 4) < 4) exit(1);
	if (write(1, out, out_len) < out_len) exit(1);   // encoded data
	free(out);

	if (len > last_start)
	    memmove(blk, &blk[last_start], len - last_start);
	blk_offset = len - last_start;
	blk_num++;
    }

    if (fclose(fp) < 0) {
	perror("closing file");
	return 1;
    }

    return 0;
}

static int decode(int argc, char **argv) {
    uint32_t in_sz, out_sz;
    while (fread(&in_sz, 1, 4, stdin) == 4) {
	uint8_t *in = malloc(in_sz), *out;
	if (!in)
	    return -1;

	if (fread(in, 1, in_sz, stdin) != in_sz) {
	    free(in);
	    return -1;
	}

	if ((out = decode_names(in, in_sz, &out_sz)) == NULL) {
	    free(in);
	    return -1;
	}

	if (write(1, out, out_sz) < out_sz) exit(1);

	free(in);
	free(out);
    }

    return 0;
}

int main(int argc, char **argv) {

    if (argc > 1 && strcmp(argv[1], "-d") == 0)
	return decode(argc-1, argv+1);
    else
	return encode(argc, argv);
}

#endif // TEST_TOKENISER
