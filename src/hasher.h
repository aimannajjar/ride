#ifndef HASHER_H
#define HASHER_H
#include <blake3.h>

#define HASH_LEN BLAKE3_OUT_LEN 
#define BUF_SIZE 65536

struct hasher {
  blake3_hasher blake;
};

int hash(struct hasher *hasher, unsigned char *out, char *file);

#endif
