/*
 * inflate.c - small DEFLATE decoder (canonical Huffman decoding bit by bit, in the style of tinf). Reads its input in
 * chunks through a callback so the compressed WAD never has to sit in RAM next to the inflated one.
 */
#include "inflate.h"

struct tree { uint16_t counts[16]; uint16_t symbols[288]; };

struct state {
    const uint8_t *src, *end;
    inflate_refill_fn refill;
    void *ctx;
    uint32_t tag;
    int bits;
    int input_error;
    uint8_t *dest, *dest_start, *dest_end;
    struct tree lt, dt;
};

static int getbit(struct state *s)
{
    if (!s->bits) {
        while (s->src == s->end) {
            if (s->refill(s->ctx, &s->src, &s->end)) { s->input_error = 1; return 0; }
        }
        s->tag = *s->src++;
        s->bits = 8;
    }
    const int b = (int)(s->tag & 1u);
    s->tag >>= 1;
    s->bits--;
    return b;
}

static unsigned read_bits(struct state *s, int n)
{
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)getbit(s) << i;
    return v;
}

static void build_tree(struct tree *t, const uint8_t *lengths, unsigned num)
{
    uint16_t offs[16];
    for (int i = 0; i < 16; i++) t->counts[i] = 0;
    for (unsigned i = 0; i < num; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    unsigned sum = 0;
    for (int i = 0; i < 16; i++) { offs[i] = (uint16_t)sum; sum += t->counts[i]; }
    for (unsigned i = 0; i < num; i++)
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

static int decode_symbol(struct state *s, const struct tree *t)
{
    int sum = 0, cur = 0, len = 0;
    do {
        cur = 2 * cur + getbit(s);
        len++;
        if (len > 15) return -1;
        sum += t->counts[len];
        cur -= t->counts[len];
    } while (cur >= 0);
    return t->symbols[sum + cur];
}

static const uint16_t len_base[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  len_bits[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t dist_base[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  dist_bits[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void fixed_trees(struct state *s)
{
    uint8_t l[288];
    unsigned i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    build_tree(&s->lt, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    build_tree(&s->dt, l, 30);
}

static int dynamic_trees(struct state *s)
{
    static const uint8_t order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    uint8_t lengths[288 + 32];
    const unsigned hlit = read_bits(s, 5) + 257, hdist = read_bits(s, 5) + 1, hclen = read_bits(s, 4) + 4;
    if (hlit > 286 || hdist > 30) return INFLATE_ERR_DATA;
    for (int i = 0; i < 19; i++) lengths[i] = 0;
    for (unsigned i = 0; i < hclen; i++) lengths[order[i]] = (uint8_t)read_bits(s, 3);
    struct tree code;
    build_tree(&code, lengths, 19);
    for (unsigned num = 0; num < hlit + hdist;) {
        const int sym = decode_symbol(s, &code);
        if (sym < 0 || s->input_error) return INFLATE_ERR_DATA;
        if (sym < 16) { lengths[num++] = (uint8_t)sym; continue; }
        unsigned rep; uint8_t prev = 0;
        if (sym == 16) { if (!num) return INFLATE_ERR_DATA; prev = lengths[num - 1]; rep = 3 + read_bits(s, 2); }
        else if (sym == 17) rep = 3 + read_bits(s, 3);
        else rep = 11 + read_bits(s, 7);
        if (num + rep > hlit + hdist) return INFLATE_ERR_DATA;
        while (rep--) lengths[num++] = prev;
    }
    build_tree(&s->lt, lengths, hlit);
    build_tree(&s->dt, lengths + hlit, hdist);
    return INFLATE_OK;
}

static int inflate_block(struct state *s)
{
    for (;;) {
        const int sym = decode_symbol(s, &s->lt);
        if (sym < 0 || s->input_error) return s->input_error ? INFLATE_ERR_INPUT : INFLATE_ERR_DATA;
        if (sym < 256) {
            if (s->dest == s->dest_end) return INFLATE_ERR_OUTPUT;
            *s->dest++ = (uint8_t)sym;
        } else if (sym == 256) {
            return INFLATE_OK;
        } else {
            const int li = sym - 257;
            if (li >= 29) return INFLATE_ERR_DATA;
            const unsigned length = len_base[li] + read_bits(s, len_bits[li]);
            const int ds = decode_symbol(s, &s->dt);
            if (ds < 0 || ds >= 30) return INFLATE_ERR_DATA;
            const unsigned dist = dist_base[ds] + read_bits(s, dist_bits[ds]);
            if (dist > (unsigned)(s->dest - s->dest_start)) return INFLATE_ERR_DATA;
            if ((size_t)(s->dest_end - s->dest) < length) return INFLATE_ERR_OUTPUT;
            const uint8_t *from = s->dest - dist;
            for (unsigned i = 0; i < length; i++) *s->dest++ = *from++;
        }
    }
}

static int stored_block(struct state *s)
{
    s->bits = 0;                                  /* discard the rest of the current byte */
    uint8_t h[4];
    for (int i = 0; i < 4; i++) {
        while (s->src == s->end) if (s->refill(s->ctx, &s->src, &s->end)) return INFLATE_ERR_INPUT;
        h[i] = *s->src++;
    }
    const unsigned len = h[0] | (h[1] << 8);
    if ((len ^ 0xffffu) != (unsigned)(h[2] | (h[3] << 8))) return INFLATE_ERR_DATA;
    for (unsigned i = 0; i < len; i++) {
        while (s->src == s->end) if (s->refill(s->ctx, &s->src, &s->end)) return INFLATE_ERR_INPUT;
        if (s->dest == s->dest_end) return INFLATE_ERR_OUTPUT;
        *s->dest++ = *s->src++;
    }
    return INFLATE_OK;
}

long inflate_raw(uint8_t *dest, size_t cap, inflate_refill_fn refill, void *ctx)
{
    static struct state s;                        /* ~3 KB: keep it off the 16 KiB stack */
    s.src = s.end = 0;
    s.refill = refill;
    s.ctx = ctx;
    s.tag = 0;
    s.bits = 0;
    s.input_error = 0;
    s.dest = s.dest_start = dest;
    s.dest_end = dest + cap;
    int last, rc = INFLATE_OK;
    do {
        last = getbit(&s);
        const unsigned type = read_bits(&s, 2);
        if (s.input_error) return INFLATE_ERR_INPUT;
        switch (type) {
        case 0: rc = stored_block(&s); break;
        case 1: fixed_trees(&s); rc = inflate_block(&s); break;
        case 2: rc = dynamic_trees(&s); if (rc == INFLATE_OK) rc = inflate_block(&s); break;
        default: rc = INFLATE_ERR_DATA;
        }
        if (rc != INFLATE_OK) return rc;
    } while (!last);
    return (long)(s.dest - s.dest_start);
}
