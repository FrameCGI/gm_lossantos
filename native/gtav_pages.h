#ifndef GTAV_PAGES_H
#define GTAV_PAGES_H

#include <stddef.h>
#include <stdint.h>

// GVPG page
// 0   magic "GVPG"
// 4   version u16
// 6   flags u16
// 8   entry count
// 12  reserved
// 16  entries, 16 bytes each (nameOff, blobOff, blobLen, reserved)
// ..  name table, then blobs

typedef struct gtav_page gtav_page;

int gtav_pages_init(const char *mapName);

int gtav_pages_set_root(const char *root);

void gtav_pages_set_budget(size_t maxMapped);

// kind v visual, p physics, 8x8 per page
const gtav_page *gtav_pages_open(char kind, int coordX, int coordY);

const uint8_t *gtav_pages_blob(const gtav_page *p, const char *fileName,
							 size_t *outLen);

uint32_t gtav_pages_count(const gtav_page *p);

int gtav_pages_enum(const gtav_page *p, uint32_t index, const char **name,
				  const uint8_t **blob, size_t *len);

void gtav_pages_trim(void);

// hold while using a blob, eviction unmaps it
void gtav_pages_lock(void);
void gtav_pages_unlock(void);

// unmap everything
void gtav_pages_shutdown(void);

// diagnostics
size_t gtav_pages_mapped(void);
const char *gtav_pages_root(void);

uint8_t *gtav_pages_read_loose(const char *subdir, const char *fileName,
							 size_t *len);

#endif /* GTAV_PAGES_H */
