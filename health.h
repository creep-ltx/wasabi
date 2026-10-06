#ifndef WASABI_HEALTH_H
#define WASABI_HEALTH_H
#include <exec/types.h>

/* Fill buf with "key value\n" lines - see health.c. Returns the length. */
LONG health_report(char *buf, LONG size);

/* Microseconds from Emu68's counter (wraps), 0 without Emu68. */
ULONG health_usecs(void);

#endif
