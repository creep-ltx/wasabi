/* Automatically generated header (sfdc 1.11e)! Do not edit! */

#ifndef CLIB_MAILBOX_PROTOS_H
#define CLIB_MAILBOX_PROTOS_H

/*
**   $VER: mailbox_protos.h 0.1.0 $Id: mailbox_lib.sfd 0.1.0 $
**
**   C prototypes. For use with 32 bit integers only.
**
**   Copyright (c) 2001 Amiga, Inc.
**       All Rights Reserved
*/

#include <exec/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */


/* "mailbox.resource" */
VOID MB_RawCommand(ULONG * command);
ULONG MB_StringCommand(STRPTR command, STRPTR reply, ULONG reply_capacity);
ULONG MB_GetClockRate(ULONG clock_id);
ULONG MB_SetClockRate(ULONG clock_id, ULONG speed);
ULONG MB_GetClockState(ULONG clock_id);
ULONG MB_SetClockState(ULONG clock_id, ULONG state);
ULONG MB_GetPowerState(ULONG id);
ULONG MB_SetPowerState(ULONG id, ULONG state);
ULONG MB_GetGPIOState(ULONG gpio);
ULONG MB_SetGPIOState(ULONG gpio, ULONG state);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CLIB_MAILBOX_PROTOS_H */
