/* Minimal streaming DEFLATE (RFC 1951) decoder for the WAD stored compressed in SPI flash. */
#ifndef INFLATE_H
#define INFLATE_H
#include <stddef.h>
#include <stdint.h>

/* Called when the input buffer runs dry: set *begin and *end to the next chunk, return 0, or non-zero at end of input/error. */
typedef int (*inflate_refill_fn)(void *ctx, const uint8_t **begin, const uint8_t **end);

enum { INFLATE_OK = 0, INFLATE_ERR_DATA = -1, INFLATE_ERR_INPUT = -2, INFLATE_ERR_OUTPUT = -3 };

/* Inflates a raw deflate stream into dest[0..cap). The output buffer doubles as the sliding window. Returns the number of
 * bytes written (>= 0) or a negative INFLATE_ERR_*. */
long inflate_raw(uint8_t *dest, size_t cap, inflate_refill_fn refill, void *ctx);
#endif
