#include "hasher.h"
#include <blake3.h>
#include <stdio.h>
#include <string.h>

void hasher_init(struct hasher *hasher) { blake3_hasher_init(&hasher->blake); }

void hasher_update(struct hasher *hasher, size_t sz,
                   const unsigned char buf[sz]) {
  blake3_hasher_update(&hasher->blake, buf, sz);
}

void hasher_finalize(struct hasher *hasher, unsigned char *out, size_t sz) {
  blake3_hasher_finalize(&hasher->blake, out, HASH_LEN);
}

