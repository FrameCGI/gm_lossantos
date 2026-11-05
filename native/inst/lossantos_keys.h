#ifndef LOSSANTOS_KEYS_H
#define LOSSANTOS_KEYS_H

#include "lossantos_crypto.h"

// aes    32 byte window of GTA5.exe matching the known sha1
// magic  CodeWalker magic.dat
// r      4 x System.Random(joaat(aes)).NextBytes
// db     aes ecb decrypt of magic - r
// out    raw inflate of db = ng keys, tables, lut, awc key
int lossantos_keys_find_aes(const wchar_t *exe, uint8_t key[32]);
int lossantos_keys_unlock(const uint8_t *magic, size_t n, const uint8_t aes[32], lossantos_ng *ng, uint8_t awc[16]);
size_t lossantos_keys_fetch_magic(uint8_t **out);
int lossantos_keys_write(const wchar_t *dir, const lossantos_ng *ng, const uint8_t aes[32], const uint8_t awc[16]);

#endif
