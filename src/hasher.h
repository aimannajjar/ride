#ifndef HASHER_H
#define HASHER_H
#include <blake3.h>

#define HASH_LEN BLAKE3_OUT_LEN 
#define BUF_SIZE 65536

struct hasher {
  blake3_hasher blake;
};

void hasher_init(struct hasher *hasher); 
void hasher_update(struct hasher *hasher, size_t sz, unsigned char buf[sz]);
void hasher_finalize(struct hasher *hasher, unsigned char *out, size_t sz);
int hash_sync(struct hasher *hasher, unsigned char *out, char *file);

#endif
