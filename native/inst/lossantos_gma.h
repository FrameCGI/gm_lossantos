#ifndef LOSSANTOS_GMA_H
#define LOSSANTOS_GMA_H

#include <stddef.h>
#include <stdint.h>

uint32_t lossantos_crc32(uint32_t crc, const void *p, size_t n);

typedef struct {
	char path[192];        /* materials/models/gtav/<region>/<stem>.vtf */
	uint32_t spill;        /* which spill file holds the bytes */
	uint64_t off;
	uint32_t size;
	uint32_t crc;
} lossantos_gma_file;

// spills = file handles, returns bytes written or 0
uint64_t lossantos_gma_write(const wchar_t *out_path, const char *title, const lossantos_gma_file *files, size_t n,
					   void *const *spills);

#endif
