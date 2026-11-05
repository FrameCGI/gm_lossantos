#ifndef GTAV_PREFETCH_H
#define GTAV_PREFETCH_H

#include <stddef.h>

int gtav_prefetch_init(const char *addon, char *err, size_t errlen);

int gtav_prefetch_add(const char *name);

typedef struct gtav_prefetch_stats {
	long long queued;     /* names accepted */
	long long done;       /* names processed */
	long long files;      /* files opened + read */
	long long missing;    /* names whose VMT was not found */
	long long dropped;    /* names refused because the queue was full */
	long long pending;    /* names waiting */
	double    ms;         /* worker wall time spent reading */
} gtav_prefetch_stats;

void gtav_prefetch_get_stats(gtav_prefetch_stats *out);

const char *gtav_prefetch_root(void);

void gtav_prefetch_shutdown(void);

#endif /* GTAV_PREFETCH_H */
