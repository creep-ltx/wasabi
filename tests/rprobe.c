/*
 * rprobe - look inside a ReAction window's layout.gadget: dump its
 * instance data, follow every pointer that looks like an exec List or a
 * BOOPSI object, and name the classes found. A research tool for
 * `wasabi ui` (which cannot see into ReAction windows yet), not part of
 * the daemon. Read-only; every pointer checked with TypeOfMem first.
 *
 *   rprobe "Execute#?"
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <intuition/classes.h>
#include <intuition/gadgetclass.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <string.h>
#include <stdlib.h>

static void ind(int d)
{
    while (d-- > 0)
        PutStr("  ");
}

static BOOL ok(const void *p)
{
    ULONG a = (ULONG)p;
    if (!a || (a & 1))
        return FALSE;
    if (a >= 0xF80000UL && a < 0x1000000UL)
        return TRUE;
    return TypeOfMem((APTR)p) != 0;
}

/* The class name of what p points at, if it is a BOOPSI object. */
static const char *objclass(const void *p)
{
    struct _Object *o;
    struct IClass *c;
    if (!ok(p) || !ok((const UBYTE *)p - sizeof(struct _Object)))
        return NULL;
    o = _OBJECT(p);
    c = o->o_Class;
    if (!ok(c))
        return NULL;
    if (!c->cl_ID) {                    /* a private class: name its parent */
        if (ok(c->cl_Super) && ok(c->cl_Super->cl_ID) &&
            ok(c->cl_Dispatcher.h_Entry))
            return "(private)";
        return NULL;
    }
    if (!ok(c->cl_ID))
        return NULL;
    {
        const UBYTE *n = (const UBYTE *)c->cl_ID;
        int k;
        for (k = 0; k < 40 && n[k]; k++)
            if (n[k] < 32 || n[k] > 126)
                return NULL;
        if (k < 3 || k >= 40)
            return NULL;
    }
    return (const char *)c->cl_ID;
}

static void text_at(const char *what, const void *p)
{
    const UBYTE *t = p;
    int k;
    if (!ok(t))
        return;
    for (k = 0; k < 60 && t[k]; k++)
        if (t[k] < 32 || (t[k] > 126 && t[k] < 160))
            return;
    if (k >= 2 && k < 60)
        Printf("        %s \"%s\"\n", (LONG)what, (LONG)t);
}

static void describe_obj(int depth, const void *o);

static void dump_gadget(int depth, struct Gadget *g)
{
    ind(depth); Printf("  box %ld,%ld %ldx%ld flags %04lx type %04lx id %ld text %08lx\n",
           (LONG)g->LeftEdge, (LONG)g->TopEdge,
           (LONG)g->Width, (LONG)g->Height, (LONG)g->Flags,
           (LONG)g->GadgetType, (LONG)g->GadgetID, (LONG)g->GadgetText);
    if (g->GadgetText) {
        if ((g->Flags & GFLG_LABELMASK) == GFLG_LABELSTRING)
            text_at("labelstring", g->GadgetText);
        else if ((g->Flags & GFLG_LABELMASK) == GFLG_LABELITEXT &&
                 ok(g->GadgetText))
            text_at("itext", g->GadgetText->IText);
        else if ((g->Flags & GFLG_LABELMASK) == GFLG_LABELIMAGE)
            describe_obj(depth + 1, g->GadgetText);
    }
}

static int seen_n;
static const void *seen[256];

static void scan_inst(int depth, const void *o, struct IClass *cl);

static void describe_obj(int depth, const void *o)
{
    const char *cn = objclass(o);
    struct IClass *cl;
    int k;
    if (!cn || depth > 6)
        return;
    for (k = 0; k < seen_n; k++)
        if (seen[k] == o)
            return;
    if (seen_n < 256)
        seen[seen_n++] = o;
    ind(depth); Printf("object %08lx class %s", (LONG)o, (LONG)cn);
    for (cl = _OBJECT(o)->o_Class->cl_Super; ok(cl); cl = cl->cl_Super)
        Printf(" < %s", (LONG)(cl->cl_ID ? cl->cl_ID : (UBYTE *)"?"));
    Printf("\n");
    /* Gadget classes start with a struct Gadget. */
    for (cl = _OBJECT(o)->o_Class; ok(cl); cl = cl->cl_Super)
        if (cl->cl_ID && strcmp((const char *)cl->cl_ID, "gadgetclass") == 0) {
            dump_gadget(depth, (struct Gadget *)o);
            break;
        }
    /* Each class level's instance data, most derived first. */
    for (cl = _OBJECT(o)->o_Class; ok(cl) && cl->cl_InstSize; cl = cl->cl_Super) {
        const char *lvl = cl->cl_ID ? (const char *)cl->cl_ID : "(private)";
        if (strcmp(lvl, "rootclass") == 0 || strcmp(lvl, "gadgetclass") == 0 ||
            strcmp(lvl, "imageclass") == 0)
            break;
        ind(depth); Printf("  [%s: %ld bytes at +%ld]\n",
               (LONG)lvl, (LONG)cl->cl_InstSize, (LONG)cl->cl_InstOffset);
        scan_inst(depth + 1, o, cl);
    }
}

static void scan_list(int depth, const struct List *l)
{
    const struct Node *n;
    int k = 0, j;
    ind(depth); Printf("list %08lx\n", (LONG)l);
    for (n = l->lh_Head; ok(n) && n->ln_Succ && k < 64; n = n->ln_Succ, k++) {
        const ULONG *w = (const ULONG *)n;
        ind(depth); Printf(" node %ld %08lx:", (LONG)k, (LONG)n);
        for (j = 0; j < 12; j++)
            Printf(" %08lx", w[j]);
        Printf("\n");
        for (j = 8; j < 44; j += 2) {
            const void *v = *(const void * const *)((const UBYTE *)n + j);
            if (objclass(v)) {
                ind(depth + 1); Printf("@node+%ld: ", (LONG)j);
                describe_obj(depth + 2, v);
            }
        }
        text_at("node name", n->ln_Name);
    }
}

static void scan_inst(int depth, const void *o, struct IClass *cl)
{
    const ULONG *d = (const ULONG *)((const UBYTE *)o + cl->cl_InstOffset);
    ULONG size = cl->cl_InstSize, i;
    if (strcmp((const char *)cl->cl_ID, "layout.gadget") == 0) {
        ind(depth); Printf("raw:");
        for (i = 0; i < size && i < 160; i += 4)
            Printf("%s%08lx", (LONG)(i % 32 ? " " : "\n    "),
                   *(const ULONG *)((const UBYTE *)d + i));
        Printf("\n");
    }
    for (i = 0; i + 12 <= size && i < 512; i += 2) {
        const ULONG *e = (const ULONG *)((const UBYTE *)d + i);
        /* an exec (Min)List kept inside the instance data */
        if (ok((const void *)e[0]) && e[1] == 0 && ok((const void *)e[2]) &&
            (e[0] != (ULONG)&e[1] || e[2] == (ULONG)&e[0])) {
            ind(depth); Printf("+%ld embedded ", (LONG)i);
            scan_list(depth, (const struct List *)e);
        }
    }
    for (i = 0; i + 4 <= size && i < 512; i += 2) {
        ULONG v = *(const ULONG *)((const UBYTE *)d + i);
        if (!ok((const void *)v))
            continue;
        if (objclass((const void *)v)) {
            ind(depth); Printf("+%ld -> ", (LONG)i);
            describe_obj(depth, (const void *)v);
        } else {
            const struct List *l = (const struct List *)v;
            if (ok(l->lh_Head) && l->lh_Tail == NULL && ok(l->lh_TailPred)) {
                ind(depth); Printf("+%ld -> ", (LONG)i);
                scan_list(depth, l);
            }
        }
    }
}

int main(int argc, char **argv)
{
    char tok[256];
    struct Screen *sc;
    struct Window *w, *hit = NULL;
    struct Gadget *g;
    ULONG ib;

    if (argc < 2) {
        Printf("rprobe PATTERN\n");
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
    Printf("window \"%s\" %ld,%ld %ldx%ld userdata %08lx\n", (LONG)hit->Title,
           (LONG)hit->LeftEdge, (LONG)hit->TopEdge, (LONG)hit->Width,
           (LONG)hit->Height, (LONG)hit->UserData);
    if (objclass(hit->UserData))
        describe_obj(0, hit->UserData);
    for (g = hit->FirstGadget; g; g = g->NextGadget) {
        Printf("gadget %08lx type %04lx flags %04lx box %ld,%ld %ldx%ld class %s\n",
               (LONG)g, (LONG)g->GadgetType, (LONG)g->Flags, (LONG)g->LeftEdge,
               (LONG)g->TopEdge, (LONG)g->Width, (LONG)g->Height,
               (LONG)(objclass(g) ? objclass(g) : "-"));
        Printf("  render %08lx (%s) select %08lx (%s)\n", (LONG)g->GadgetRender,
               (LONG)(objclass(g->GadgetRender) ? objclass(g->GadgetRender) : "-"),
               (LONG)g->SelectRender,
               (LONG)(objclass(g->SelectRender) ? objclass(g->SelectRender) : "-"));
        Printf("  userdata %08lx (%s) special %08lx (%s)\n", (LONG)g->UserData,
               (LONG)(objclass(g->UserData) ? objclass(g->UserData) : "-"),
               (LONG)g->SpecialInfo,
               (LONG)(objclass(g->SpecialInfo) ? objclass(g->SpecialInfo) : "-"));
        if (objclass(g) && !getenv("NODESC"))
            describe_obj(0, g);
    }
    Printf("requests %08lx firstreq %08lx\n", (LONG)hit->ReqCount, (LONG)hit->FirstRequest);
    return 0;
}
