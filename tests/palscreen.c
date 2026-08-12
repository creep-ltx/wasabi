/*
 * palscreen - open a plain PAL 640x256 hires screen for a few seconds,
 * so `wasabi grab` can be tested against a real native non-interlaced
 * screen: its pixels are 2:1 and the client must double the rows to
 * get a 4:3 image out. The DisplayID is given explicitly so an RTG
 * Workbench cannot promote the screen to a square-pixel mode behind
 * the test's back.
 *
 *   palscreen [seconds]     default 10
 */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <intuition/screens.h>
#include <graphics/modeid.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 10;
    struct Screen *sc = OpenScreenTags(NULL,
        SA_Width,     640,
        SA_Height,    256,
        SA_Depth,     2,
        SA_DisplayID, PAL_MONITOR_ID | HIRES_KEY,
        SA_Title,     (ULONG)"wasabi palscreen",
        SA_ShowTitle, TRUE,
        TAG_DONE);
    if (!sc)
        return 20;
    Delay(secs * 50);
    CloseScreen(sc);
    return 0;
}
