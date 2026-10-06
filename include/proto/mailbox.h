/* Automatically generated header (sfdc 1.11e)! Do not edit! */

#ifndef PROTO_MAILBOX_H
#define PROTO_MAILBOX_H

#include <clib/mailbox_protos.h>

#if defined(_CONST_BASES)
# ifndef __CONSTLIBBASEDECL__
# define __CONSTLIBBASEDECL__ const
# endif /* __CONSTLIBBASEDECL__ */
# ifndef __SEGMENTLIBBASEDECL__
# define __SEGMENTLIBBASEDECL__  __attribute__((__section__(".data")))
# endif /* __SEGMENTLIBBASEDECL__ */
#endif /* _CONST_BASES */
#ifdef __amigaos4__
# include <interfaces/mailbox.h>
# ifndef __NOGLOBALIFACE__
   extern struct MailboxIFace *IMailbox;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern APTR
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  MailboxBase
# ifdef __SEGMENTLIBBASEDECL__
 __SEGMENTLIBBASEDECL__
# endif /* __SEGMENTLIBBASEDECL__ */
;
#endif /* !__NOLIBBASE__ */

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/mailbox.h>
#  else
#   include <inline/mailbox.h>
#  endif
# else
#  include <pragmas/mailbox_pragmas.h>
# endif
#endif /* _NO_INLINE */

#endif /* !PROTO_MAILBOX_H */
