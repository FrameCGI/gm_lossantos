#ifndef LOSSANTOS_CRYPTO_H
#define LOSSANTOS_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

#define LOSSANTOS_NG_KEYS 101
#define LOSSANTOS_NG_KEY_SIZE 272
#define LOSSANTOS_NG_ROUNDS 17

#define LOSSANTOS_ENC_NONE 0u
#define LOSSANTOS_ENC_OPEN 0x4E45504Fu  // "OPEN"
#define LOSSANTOS_ENC_AES 0x0FFFFFF9u
#define LOSSANTOS_ENC_NG 0x0FEFFFFFu

typedef struct {
	uint8_t keys[LOSSANTOS_NG_KEYS][LOSSANTOS_NG_KEY_SIZE];  // 17 * 4 u32 round keys
	uint32_t tables[LOSSANTOS_NG_ROUNDS][16][256];  // [round][byte pos][byte value]
	uint8_t lut[256];
	uint8_t aes[32];         /* legacy AES archives (gtav_aes_key.dat) */
	int has_aes;
} lossantos_ng;

int lossantos_aes_decrypt(const uint8_t key[32], uint8_t *data, size_t len);

int lossantos_ng_load(lossantos_ng *ng, const wchar_t *dir);

uint32_t lossantos_name_hash(const lossantos_ng *ng, const char *name);
const uint8_t *lossantos_key_for(const lossantos_ng *ng, const char *name, uint32_t size);

void lossantos_ng_decrypt(const lossantos_ng *ng, uint8_t *data, size_t len, const uint8_t *key);

int lossantos_toc_shaped(const uint8_t *head16, uint32_t entry_count);

#endif
