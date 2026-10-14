/*
 * mprobe - look inside an MUI window: find its MUI window object, walk
 * the object tree from MUIA_Window_RootObject through every group's
 * MUIA_Group_ChildList, and print each object's class, box (the public
 * _left/_top/_width/_height area data) and text. A research tool for
 * `wasabi ui`, not part of the daemon.
 *
 *   mprobe "MUI-Demo#?"
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/classes.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>

#define MUIA_Window_RootObject 0x8042cba5
#define MUIA_Group_ChildList   0x80424748
#define MUIA_Text_Contents     0x8042f8dc
#define MUIA_String_Contents   0x80428ffd
#define MUIA_InputMode         0x8042fb04
#define MUIA_Selected          0x8042654b
#define MUIA_Disabled          0x80423661
#define MUIA_Window_Window     0x80426a42
#define MUIA_Cycle_Active      0x80421788
#define MUIA_Cycle_Entries     0x80420629
#define MUIA_FrameTitle        0x8042d1c7

static void ind(int d) { while (d-- > 0) PutStr("  "); }

static BOOL ok(const void *p)
{
    ULONG a = (ULONG)p;
    if (!a || (a & 1))
        return FALSE;
    if (a >= 0xF80000UL && a < 0x1000000UL)
        return TRUE;
    return TypeOfMem((APTR)p) != 0;
}

static const char *cname(struct IClass *c)
{
    if (!ok(c))
        return NULL;
    return c->cl_ID && ok(c->cl_ID) ? (const char *)c->cl_ID : "(private)";
}

static BOOL is_obj(const void *p)
{
    struct IClass *c;
    if (!ok(p) || !ok((const UBYTE *)p - sizeof(struct _Object)))
        return FALSE;
    c = _OBJECT(p)->o_Class;
    return ok(c) && ok(c->cl_Dispatcher.h_Entry);
}

static BOOL has_class(const void *o, const char *name)
{
    struct IClass *c;
    for (c = _OBJECT(o)->o_Class; ok(c); c = c->cl_Super)
        if (c->cl_ID && ok(c->cl_ID) && strcmp((const char *)c->cl_ID, name) == 0)
            return TRUE;
    return FALSE;
}

static void text(const char *what, ULONG v)
{
    const UBYTE *t = (const UBYTE *)v;
    int k;
    if (!ok(t))
        return;
    for (k = 0; k < 80 && t[k]; k++)
        if (t[k] < 32 && t[k] != 27)
            return;
    Printf(" %s=\"%.60s\"", (LONG)what, (LONG)t);
}

static int count;

static void walk(int depth, Object *o)
{
    struct IClass *c;
    ULONG v = 0;
    const WORD *box = (const WORD *)((const UBYTE *)o + 52);
    if (depth > 12 || ++count > 400 || !is_obj(o))
        return;
    ind(depth);
    Printf("%08lx", (LONG)o);
    for (c = _OBJECT(o)->o_Class; ok(c); c = c->cl_Super)
        Printf(" %s", (LONG)cname(c));
    if (has_class(o, "Area.mui"))
        Printf(" box %ld,%ld %ldx%ld", (LONG)box[0], (LONG)box[1],
               (LONG)box[2], (LONG)box[3]);
    if (has_class(o, "Text.mui") && GetAttr(MUIA_Text_Contents, o, &v))
        text("text", v);
    if (has_class(o, "String.mui") && GetAttr(MUIA_String_Contents, o, &v))
        text("string", v);
    if (has_class(o, "Area.mui")) {
        if (GetAttr(MUIA_InputMode, o, &v) && v)
            Printf(" input=%ld", (LONG)v);
        if (GetAttr(MUIA_Selected, o, &v) && v)
            Printf(" selected");
        if (GetAttr(MUIA_Disabled, o, &v) && v)
            Printf(" disabled");
        if (GetAttr(MUIA_FrameTitle, o, &v) && v)
            text("frametitle", v);
    }
    if (has_class(o, "Cycle.mui") && GetAttr(MUIA_Cycle_Active, o, &v)) {
        ULONG e = 0;
        Printf(" active=%ld", (LONG)v);
        if (GetAttr(MUIA_Cycle_Entries, o, &e) && ok((void *)e))
            text("entry", ((ULONG *)e)[v]);
    }
    Printf("\n");
    if (has_class(o, "Group.mui")) {
        struct List *l = NULL;
        if (GetAttr(MUIA_Group_ChildList, o, (ULONG *)&l) && ok(l)) {
            struct Node *n;
            int k = 0;
            for (n = l->lh_Head; ok(n) && n->ln_Succ && k < 200;
                 n = n->ln_Succ, k++)
                walk(depth + 1, (Object *)((struct _Object *)n + 1));
        }
    }
}

int main(int argc, char **argv)
{
    char tok[256];
    struct Screen *sc;
    struct Window *w, *hit = NULL;
    ULONG ib, root = 0;

    if (argc < 2) {
        Printf("mprobe PATTERN\n");
        return 5;
    }
    ParsePatternNoCase(argv[1], tok, sizeof tok);
    ib = LockIBase(0);
    for (sc = IntuitionBase->FirstScreen; sc && !hit; sc = sc->NextScreen)
        for (w = sc->FirstWindow; w && !hit; w = w->NextWindow)
            if (w->Title && MatchPatternNoCase(tok, w->Title))
                hit = w;
    UnlockIBase(ib);
    if (!hit) {
        Printf("no window\n");
        return 5;
    }
    Printf("window \"%s\" userdata %08lx\n", (LONG)hit->Title,
           (LONG)hit->UserData);
    {
        /* Find the Window.mui object: it holds this window's address in
         * its instance data. Scan RAM for the value, verify each hit. */
        struct MemHeader *mh;
        Object *found = NULL;
        ULONG t0 = 0, hits = 0;
        struct DateStamp d0, d1;
        DateStamp(&d0);
        Forbid();
        for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
             mh->mh_Node.ln_Succ && !found;
             mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
            ULONG *p = (ULONG *)(((ULONG)mh->mh_Lower + 3) & ~3UL);
            ULONG *e = (ULONG *)mh->mh_Upper;
            for (; p < e && !found; p++)
                if (*p == (ULONG)hit) {
                    LONG dd;
                    hits++;
                    /* struct MUI_RenderInfo: WindowObject, Screen,
                     * DrawInfo, Pens, Window - so p is mri_Window when
                     * p[-3] is its screen and p[-4] a Window.mui. */
                    if (p[-3] == (ULONG)hit->WScreen &&
                        is_obj((void *)p[-4]) &&
                        has_class((void *)p[-4], "Window.mui"))
                        found = (Object *)p[-4];
                }
        }
        Permit();
        DateStamp(&d1);
        t0 = (d1.ds_Minute - d0.ds_Minute) * 3000 + d1.ds_Tick - d0.ds_Tick;
        Printf("scan: %ld hits, %ld ticks, window object %08lx\n", (LONG)hits,
               (LONG)t0, (LONG)found);
        if (!found)
            return 10;
        hit->UserData = hit->UserData;   /* untouched */
        {
            ULONG rootx = 0;
            if (GetAttr(MUIA_Window_RootObject, found, &rootx) && rootx)
                walk(0, (Object *)rootx);
        }
        return 0;
    }
    {
        struct IClass *c;
        Printf("userdata class:");
        for (c = _OBJECT(hit->UserData)->o_Class; ok(c); c = c->cl_Super)
            Printf(" %s", (LONG)cname(c));
        Printf("\n");
    }
    if (!has_class(hit->UserData, "Window.mui")) {
        Printf("not an MUI window\n");
        return 10;
    }
    if (GetAttr(MUIA_Window_RootObject, hit->UserData, &root) && root)
        walk(0, (Object *)root);
    return 0;
}
