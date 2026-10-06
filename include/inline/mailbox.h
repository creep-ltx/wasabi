/* Automatically generated header (sfdc 1.11e)! Do not edit! */

#ifndef _INLINE_MAILBOX_H
#define _INLINE_MAILBOX_H

#ifndef _SFDC_VARARG_DEFINED
#define _SFDC_VARARG_DEFINED
#ifdef __HAVE_IPTR_ATTR__
typedef APTR _sfdc_vararg __attribute__((iptr));
#else
typedef ULONG _sfdc_vararg;
#endif /* __HAVE_IPTR_ATTR__ */
#endif /* _SFDC_VARARG_DEFINED */

#ifndef __INLINE_MACROS_H
#include <inline/macros.h>
#endif /* !__INLINE_MACROS_H */

#ifndef MAILBOX_BASE_NAME
#define MAILBOX_BASE_NAME MailboxBase
#endif /* !MAILBOX_BASE_NAME */

#define MB_RawCommand(___command) \
      LP1NR(0x6, MB_RawCommand , ULONG *, ___command, a0,\
      , MAILBOX_BASE_NAME)

#define MB_StringCommand(___command, ___reply, ___reply_capacity) \
      LP3(0xc, ULONG, MB_StringCommand , STRPTR, ___command, a0, STRPTR, ___reply, a1, ULONG, ___reply_capacity, d0,\
      , MAILBOX_BASE_NAME)

#define MB_GetClockRate(___clock_id) \
      LP1(0x12, ULONG, MB_GetClockRate , ULONG, ___clock_id, d0,\
      , MAILBOX_BASE_NAME)

#define MB_SetClockRate(___clock_id, ___speed) \
      LP2(0x18, ULONG, MB_SetClockRate , ULONG, ___clock_id, d0, ULONG, ___speed, d1,\
      , MAILBOX_BASE_NAME)

#define MB_GetClockState(___clock_id) \
      LP1(0x1e, ULONG, MB_GetClockState , ULONG, ___clock_id, d0,\
      , MAILBOX_BASE_NAME)

#define MB_SetClockState(___clock_id, ___state) \
      LP2(0x24, ULONG, MB_SetClockState , ULONG, ___clock_id, d0, ULONG, ___state, d1,\
      , MAILBOX_BASE_NAME)

#define MB_GetPowerState(___id) \
      LP1(0x2a, ULONG, MB_GetPowerState , ULONG, ___id, d0,\
      , MAILBOX_BASE_NAME)

#define MB_SetPowerState(___id, ___state) \
      LP2(0x30, ULONG, MB_SetPowerState , ULONG, ___id, d0, ULONG, ___state, d1,\
      , MAILBOX_BASE_NAME)

#define MB_GetGPIOState(___gpio) \
      LP1(0x36, ULONG, MB_GetGPIOState , ULONG, ___gpio, d0,\
      , MAILBOX_BASE_NAME)

#define MB_SetGPIOState(___gpio, ___state) \
      LP2(0x3c, ULONG, MB_SetGPIOState , ULONG, ___gpio, d0, ULONG, ___state, d1,\
      , MAILBOX_BASE_NAME)

#endif /* !_INLINE_MAILBOX_H */
