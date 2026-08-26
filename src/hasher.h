#ifndef HASHER_H
#define HASHER_H
#include <blake3.h>
#include <stdio.h>

#define HASH_LEN BLAKE3_OUT_LEN

int hash(unsigned char *out, FILE *fp);

#endif
