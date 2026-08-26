#include <blake3.h>
#include <stdio.h>
#include "hasher.h"

#define BUF_SIZE 65536

unsigned char buf[BUF_SIZE];
blake3_hasher hasher;

static void hasher_init() { blake3_hasher_init(&hasher); }

static void hasher_finalize() {
  blake3_hasher_finalize(&hasher, buf, HASH_LEN);
}

/** hashes file `fp` and stores the result in `out`
 ** out must be 32-byte buffer. returns 0 on success
 ** nonzero on errors
 */
int hash(unsigned char *out, FILE *fp) {
  hasher_init();
  unsigned char buf[1024];
  while (1) {
    size_t n = fread(buf, 1, 1024, fp);
    if (ferror(fp)) {
      perror("fread");
      return -1;
    }

    blake3_hasher_update(&hasher, buf, n);

    if (n < 1024)
      break;
  }

  blake3_hasher_finalize(&hasher, out, HASH_LEN);
  return 0;
}

