/* Automatically generated header (sfdc 1.11e)! Do not edit! */

#ifndef PROTO_DEVICETREE_H
#define PROTO_DEVICETREE_H

#include <clib/devicetree_protos.h>

#if defined(_CONST_BASES)
# ifndef __CONSTLIBBASEDECL__
# define __CONSTLIBBASEDECL__ const
# endif /* __CONSTLIBBASEDECL__ */
# ifndef __SEGMENTLIBBASEDECL__
# define __SEGMENTLIBBASEDECL__  __attribute__((__section__(".data")))
# endif /* __SEGMENTLIBBASEDECL__ */
#endif /* _CONST_BASES */
#ifdef __amigaos4__
# include <interfaces/devicetree.h>
# ifndef __NOGLOBALIFACE__
   extern struct DeviceTreeIFace *IDeviceTree;
# endif /* __NOGLOBALIFACE__*/
#endif /* !__amigaos4__ */
#ifndef __NOLIBBASE__
  extern APTR
# ifdef __CONSTLIBBASEDECL__
   __CONSTLIBBASEDECL__
# endif /* __CONSTLIBBASEDECL__ */
  DeviceTreeBase
# ifdef __SEGMENTLIBBASEDECL__
 __SEGMENTLIBBASEDECL__
# endif /* __SEGMENTLIBBASEDECL__ */
;
#endif /* !__NOLIBBASE__ */

#ifndef _NO_INLINE
# if defined(__GNUC__)
#  ifdef __AROS__
#   include <defines/devicetree.h>
#  else
#   include <inline/devicetree.h>
#  endif
# else
#  include <pragmas/devicetree_pragmas.h>
# endif
#endif /* _NO_INLINE */

#endif /* !PROTO_DEVICETREE_H */
