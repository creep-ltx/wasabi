/*
 * gtwin - a small GadTools window for testing `wasabi ui`: a button, a
 * text field, a checkbox, a cycle gadget and a menu strip with a
 * shortcut, a checkmark item and a sub-menu. Everything it hears goes
 * to its output, one line each, so `wasabi run gtwin` shows what a
 * click or a menu pick actually delivered. Close gadget or Project/Quit
 * ends it.
 *
 * Build: m68k-amigaos-gcc -O2 -fbbb=- -noixemul -o gtwin gtwin.c
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <libraries/gadtools.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

struct Library *GadToolsBase;

enum { G_BUTTON = 1, G_TEXT, G_CHECK, G_CYCLE };

static STRPTR cycle_labels[] = { "Red", "Green", "Blue", NULL };

static struct NewMenu menus[] = {
    { NM_TITLE, "Project",      NULL, 0, 0, NULL },
    { NM_ITEM,  "Open...",      "O",  0, 0, NULL },
    { NM_ITEM,  "Recent",       NULL, 0, 0, NULL },
    { NM_SUB,   "First file",   NULL, 0, 0, NULL },
    { NM_SUB,   "Second file",  NULL, 0, 0, NULL },
    { NM_ITEM,  NM_BARLABEL,    NULL, 0, 0, NULL },
    { NM_ITEM,  "Quit",         "Q",  0, 0, NULL },
    { NM_TITLE, "Settings",     NULL, 0, 0, NULL },
    { NM_ITEM,  "Loud",         NULL, CHECKIT | MENUTOGGLE, 0, NULL },
    { NM_ITEM,  "Disabled one", NULL, NM_ITEMDISABLED, 0, NULL },
    { NM_END,   NULL,           NULL, 0, 0, NULL }
};

int main(void)
{
    struct Screen *scr;
    APTR vi;
    struct Gadget *glist = NULL, *g, *text;
    struct NewGadget ng = {0};
    struct Window *win;
    struct Menu *strip;
    BOOL done = FALSE;

    if (!(GadToolsBase = OpenLibrary("gadtools.library", 37)))
        return 20;
    scr = LockPubScreen(NULL);
    vi = GetVisualInfo(scr, TAG_END);
    g = CreateContext(&glist);

    ng.ng_VisualInfo = vi;
    ng.ng_TextAttr = scr->Font;
    ng.ng_LeftEdge = 90;
    ng.ng_TopEdge = scr->WBorTop + scr->Font->ta_YSize + 8;
    ng.ng_Width = 160;
    ng.ng_Height = 14;

    ng.ng_GadgetText = "_Name";
    ng.ng_GadgetID = G_TEXT;
    ng.ng_Flags = PLACETEXT_LEFT;
    text = g = CreateGadget(STRING_KIND, g, &ng, GTST_MaxChars, 60,
                            GT_Underscore, '_', TAG_END);

    ng.ng_TopEdge += 20;
    ng.ng_GadgetText = "Colour";
    ng.ng_GadgetID = G_CYCLE;
    g = CreateGadget(CYCLE_KIND, g, &ng, GTCY_Labels, cycle_labels, TAG_END);

    ng.ng_TopEdge += 20;
    ng.ng_Width = 26;
    ng.ng_Height = 11;
    ng.ng_GadgetText = "Loud";
    ng.ng_GadgetID = G_CHECK;
    g = CreateGadget(CHECKBOX_KIND, g, &ng, TAG_END);

    ng.ng_TopEdge += 20;
    ng.ng_Width = 100;
    ng.ng_Height = 14;
    ng.ng_GadgetText = "_Save";
    ng.ng_GadgetID = G_BUTTON;
    ng.ng_Flags = PLACETEXT_IN;
    g = CreateGadget(BUTTON_KIND, g, &ng, GT_Underscore, '_', TAG_END);

    strip = CreateMenus(menus, TAG_END);
    if (!g || !strip || !LayoutMenus(strip, vi, GTMN_NewLookMenus, TRUE,
                                     TAG_END))
        return 20;

    win = OpenWindowTags(NULL,
        WA_Title, "gtwin test",
        WA_Left, 120, WA_Top, 40,
        WA_Width, 300, WA_Height, ng.ng_TopEdge + 26,
        WA_Gadgets, glist,
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE,
        WA_Activate, TRUE, WA_NewLookMenus, TRUE,
        WA_PubScreen, scr,
        WA_IDCMP, IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_REFRESHWINDOW |
                  BUTTONIDCMP | STRINGIDCMP | CHECKBOXIDCMP | CYCLEIDCMP,
        TAG_END);
    UnlockPubScreen(NULL, scr);
    if (!win)
        return 20;
    SetMenuStrip(win, strip);
    GT_RefreshWindow(win, NULL);
    Printf("gtwin: open\n");
    Flush(Output());

    while (!done) {
        struct IntuiMessage *im;
        WaitPort(win->UserPort);
        while ((im = GT_GetIMsg(win->UserPort))) {
            ULONG cls = im->Class;
            UWORD code = im->Code;
            struct Gadget *gad = (struct Gadget *)im->IAddress;
            GT_ReplyIMsg(im);
            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                Printf("gtwin: close\n");
                done = TRUE;
                break;
            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                break;
            case IDCMP_GADGETUP:
                if (gad->GadgetID == G_TEXT)
                    Printf("gtwin: text \"%s\"\n", (LONG)((struct StringInfo *)
                           text->SpecialInfo)->Buffer);
                else
                    Printf("gtwin: gadget %ld code %ld\n",
                           (LONG)gad->GadgetID, (LONG)code);
                break;
            case IDCMP_MENUPICK:
                while (code != MENUNULL) {
                    struct MenuItem *it = ItemAddress(strip, code);
                    if (!it)
                        break;
                    Printf("gtwin: menu %ld/%ld/%ld%s\n", (LONG)MENUNUM(code),
                           (LONG)ITEMNUM(code), (LONG)SUBNUM(code),
                           (LONG)((it->Flags & CHECKED) ? " checked" : ""));
                    if (MENUNUM(code) == 0 && ITEMNUM(code) == 3)
                        done = TRUE;
                    code = it->NextSelect;
                }
                break;
            }
            Flush(Output());
        }
    }
    ClearMenuStrip(win);
    CloseWindow(win);
    FreeMenus(strip);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    CloseLibrary(GadToolsBase);
    return 0;
}
