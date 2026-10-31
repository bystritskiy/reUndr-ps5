#ifndef NFSU1_SHA256_H
#define NFSU1_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* SHA-256 (FIPS 180-4): the game checks default.xbe against NFSU1_XBE_SHA256
 * before mapping it. */
typedef struct {
    uint32_t state[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} Nfsu1Sha256;

void nfsu1_sha256_init(Nfsu1Sha256 *c);
void nfsu1_sha256_update(Nfsu1Sha256 *c, const void *data, size_t size);
/* Upper-case hexadecimal, as in game_revision.h. */
void nfsu1_sha256_hex(Nfsu1Sha256 *c, char hex[65]);
/* 1 and the digest of a whole file, 0 when it cannot be read. */
int nfsu1_sha256_file(const char *path, char hex[65]);

#endif
