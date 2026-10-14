/*
 * wasabi_cgx.h - the little of cybergraphics.library wasabid uses: read
 * pixels from a deep (RTG) screen, and lock its bitmap to read it in
 * place. Declared here so that wasabid builds with any m68k toolchain,
 * with or without the CyberGraphX/Picasso96 developer headers (Spot
 * found 0.8 needed them). The values are the library's interface:
 * LVOs, registers and constants as the library defines them.
 */
#ifndef WASABI_CGX_H
#define WASABI_CGX_H

#include <exec/types.h>
#include <graphics/rastport.h>
#include <utility/tagitem.h>

/* pixel formats (LBMI_PIXFMT) */
#define PIXFMT_RGB16    (5UL)
#define PIXFMT_RGB16PC  (7UL)
#define PIXFMT_RGB24    (9UL)
#define PIXFMT_BGR24    (10UL)
#define PIXFMT_ARGB32   (11UL)
#define PIXFMT_BGRA32   (12UL)
#define PIXFMT_RGBA32   (13UL)

/* ReadPixelArray() destination formats */
#define RECTFMT_RGB     (0UL)

/* LockBitMapTagList() */
#define LBMI_PIXFMT      (0x84001004UL)
#define LBMI_BYTESPERROW (0x84001006UL)
#define LBMI_BASEADDRESS (0x84001007UL)

/* ReadPixelArray(destRect a0, destX d0, destY d1, destMod d2,
 *                RastPort a1, SrcX d3, SrcY d4, SizeX d5, SizeY d6,
 *                DestFormat d7) = LVO -120 */
#define __ReadPixelArray_base(base, dr, dx, dy, dm, rp, sx, sy, w, h, fmt) ({\
  register void *_a6 __asm("a6") = (void *)(base);                      \
  register ULONG _r __asm("d0");                                         \
  register APTR _a0 __asm("a0") = (APTR)(dr);                           \
  register ULONG _d0 __asm("d0") = (UWORD)(dx);                          \
  register ULONG _d1 __asm("d1") = (UWORD)(dy);                          \
  register ULONG _d2 __asm("d2") = (UWORD)(dm);                          \
  register struct RastPort *_a1 __asm("a1") = (rp);                      \
  register ULONG _d3 __asm("d3") = (UWORD)(sx);                          \
  register ULONG _d4 __asm("d4") = (UWORD)(sy);                          \
  register ULONG _d5 __asm("d5") = (UWORD)(w);                           \
  register ULONG _d6 __asm("d6") = (UWORD)(h);                           \
  register ULONG _d7 __asm("d7") = (UBYTE)(fmt);                         \
  __asm volatile ("jsr %%a6@(-120:W)"                                    \
    : "=d"(_r), "+a"(_a0), "+d"(_d1), "+a"(_a1)                          \
    : "a"(_a6), "0"(_d0), "d"(_d2), "d"(_d3), "d"(_d4), "d"(_d5),        \
      "d"(_d6), "d"(_d7)                                                 \
    : "fp0", "fp1", "cc", "memory");                                     \
  _r; })

/* LockBitMapTagList(BitMap a0, TagList a1) = LVO -168 -> handle */
#define __LockBitMapTagList_base(base, bm, tags) ({                     \
  register void *_a6 __asm("a6") = (void *)(base);                      \
  register APTR _r __asm("d0");                                          \
  register APTR _a0 __asm("a0") = (APTR)(bm);                           \
  register struct TagItem *_a1 __asm("a1") = (tags);                     \
  __asm volatile ("jsr %%a6@(-168:W)"                                    \
    : "=d"(_r), "+a"(_a0), "+a"(_a1)                                     \
    : "a"(_a6)                                                           \
    : "d1", "fp0", "fp1", "cc", "memory");                               \
  _r; })

/* UnLockBitMap(Handle a0) = LVO -174 */
#define __UnLockBitMap_base(base, handle) ({                            \
  register void *_a6 __asm("a6") = (void *)(base);                      \
  register APTR _a0 __asm("a0") = (APTR)(handle);                       \
  __asm volatile ("jsr %%a6@(-174:W)"                                    \
    : "+a"(_a0)                                                          \
    : "a"(_a6)                                                           \
    : "d0", "d1", "a1", "fp0", "fp1", "cc", "memory");                   \
  })

#endif
