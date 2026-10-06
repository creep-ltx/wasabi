#ifndef WASABI_HEALTH_H
#define WASABI_HEALTH_H
#include <exec/types.h>

/* Fill buf with "key value\n" lines - see health.c. Returns the length. */
LONG health_report(char *buf, LONG size);

#endif
