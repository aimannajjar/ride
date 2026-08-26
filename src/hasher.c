#include "hasher.h"
#include <blake3.h>
#include <stdio.h>
#include <string.h>

/** hashes file `file` and stores the result in `out`
 ** out must be 32-byte buffer. returns 0 on success
 ** nonzero on errors
 */
int hash(struct hasher *hasher, unsigned char *out, char *file) {
  FILE *fp = fopen(file, "rb");
  if (fp == NULL) {
    perror("open");
    return 1;
  }

  blake3_hasher_init(&hasher->blake);
  unsigned char buf[1024];
  while (1) {
    size_t n = fread(buf, 1, 1024, fp);
    if (ferror(fp)) {
      perror("fread");
      fclose(fp);
      return 2;
    }

    blake3_hasher_update(&hasher->blake, buf, n);

    if (n < 1024)
      break;
  }

  blake3_hasher_finalize(&hasher->blake, out, HASH_LEN);
  fclose(fp);
  return 0;
}
