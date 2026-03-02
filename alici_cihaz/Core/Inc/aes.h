#ifndef _AES_H_
#define _AES_H_

#include <stdint.h>

#define AES256 1
#define AES_BLOCKLEN 16

struct AES_ctx {
  uint8_t RoundKey[240]; // AES-256 için 15 round key * 16 byte
  uint8_t Iv[16];
};

void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, const uint8_t* iv);
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, uint32_t length);

#endif
