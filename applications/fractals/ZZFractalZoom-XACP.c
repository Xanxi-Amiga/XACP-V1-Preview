/*
 * ZZFractalZoom_XACP.c
 * ====================
 * Demo 2 XACP / ZZ9000
 *
 * ARM ZZ9000 calcule une sequence de Mandelbrot zoom
 * dans un buffer offscreen, puis le 68k affiche chaque frame.
 *
 * Compatible firmware MHIXX5 :
 * - utilise uniquement OP_MANDELBROT = 2
 * - aucun nouveau registre
 * - aucun nouveau BOOT.bin
 *
 * Build:
 *   m68k-amigaos-gcc -O2 -noixemul -m68020 \
 *       -o ZZFractalZoom_XACP ZZFractalZoom_XACP.c -lamiga
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/expansion.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define ZZ9000_MANUF     0x6D6E
#define ZZ9000_PROD      0x04

#define MNT_FB_BASE      0x00010000UL
#define XACP_CMD_OFFSET  0x04000000UL
#define XACP_OUT_OFFSET  0x04080000UL

#define REG_CMD          0x64
#define OP_MANDELBROT    2
#define STATUS_DONE      2
#define STATUS_ERROR     3

#define IMG_W            320
#define IMG_H            240
#define STATUS_H         20
#define MAXITER          96
#define NUM_FRAMES       32

#define ZZ_WR(b,o,v) (*((volatile UWORD*)((UBYTE*)(b)+(o))) = (UWORD)(v))
#define ZZ_RD(b,o)   (*((volatile UWORD*)((UBYTE*)(b)+(o))))

typedef struct {
    ULONG opcode;
    ULONG framebuffer_ptr;
    ULONG pitch_bytes;
    ULONG width;
    ULONG height;
    ULONG max_iter;
    LONG  xmin_fp;
    LONG  xmax_fp;
    LONG  ymin_fp;
    LONG  ymax_fp;
} XACP_Command;

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *CyberGfxBase  = NULL;

/* Q16.16 helpers */
#define Q16_ONE 65536L

static void draw_status(struct RastPort *rp, WORD x, WORD y, WORD w, const char *msg)
{
    SetAPen(rp, 0);
    RectFill(rp, x, y, x + w - 1, y + STATUS_H - 1);
    SetAPen(rp, 1);
    Move(rp, x + 4, y + rp->Font->tf_Baseline + 2);
    Text(rp, msg, (UWORD)strlen(msg));
}

static ULONG elapsed_ms(ULONG s0, ULONG u0, ULONG s1, ULONG u1)
{
    return (s1 - s0) * 1000UL +
           (u1 >= u0 ? (u1 - u0) / 1000UL
                     : 1000UL - (u0 - u1) / 1000UL);
}

/*
 * Zoom tour autour de Seahorse Valley.
 * centre approx: -0.743643, 0.131825
 *
 * On évite les floats : tout en Q16.16.
 */
static void make_view(WORD frame, LONG *xmin, LONG *xmax, LONG *ymin, LONG *ymax)
{
    LONG cx = -48731; /* -0.743643 * 65536 */
    LONG cy =   8641; /*  0.131825 * 65536 */

    /*
     * Demi-largeur de départ ≈ 2.0 en Q16.
     * On réduit progressivement par facteur approximatif 5/6.
     */
    LONG half_w = 2L * Q16_ONE;
    LONG half_h;
    WORD i;

    for (i = 0; i < frame; i++) {
        half_w = (half_w * 5L) / 6L;
        if (half_w < 512)
            half_w = 512;
    }

    /* ratio 320x240 = 4:3 */
    half_h = (half_w * 3L) / 4L;

    *xmin = cx - half_w;
    *xmax = cx + half_w;
    *ymin = cy - half_h;
    *ymax = cy + half_h;
}

int main(void)
{
    struct ConfigDev *cd = NULL;
    volatile UBYTE *board = NULL;
    volatile UBYTE *fb = NULL;
    XACP_Command *xcmd = NULL;

    struct Window *win = NULL;
    struct RastPort *rp = NULL;

    ULONG s0, u0, s1, u1;
    ULONG total_ms = 0;
    WORD frame;
    char msg[128];
    int rc = 0;

    static UBYTE title[] = "ZZFractal Zoom XACP - ARM Mandelbrot Tour";

    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37L);
    GfxBase       = (struct GfxBase *)OpenLibrary("graphics.library", 37L);
    CyberGfxBase  = OpenLibrary("cybergraphics.library", 40L);

    if (!IntuitionBase || !GfxBase || !CyberGfxBase) {
        printf("Cannot open required libraries.\n");
        rc = 20;
        goto bye;
    }

    ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37L);
    if (!ExpansionBase) {
        printf("Cannot open expansion.library.\n");
        rc = 20;
        goto bye;
    }

    cd = FindConfigDev(NULL, ZZ9000_MANUF, ZZ9000_PROD);
    if (!cd) cd = FindConfigDev(NULL, ZZ9000_MANUF, 0x02);
    if (!cd) cd = FindConfigDev(NULL, ZZ9000_MANUF, 0x01);

    CloseLibrary((struct Library *)ExpansionBase);
    ExpansionBase = NULL;

    if (!cd) {
        printf("ZZ9000 not found.\n");
        rc = 10;
        goto bye;
    }

    board = (volatile UBYTE *)cd->cd_BoardAddr;
    fb    = board + MNT_FB_BASE;
    xcmd  = (XACP_Command *)(fb + XACP_CMD_OFFSET);

    {
        struct Screen *pub = LockPubScreen(NULL);
        WORD sw = pub ? (WORD)pub->Width  : 640;
        WORD sh = pub ? (WORD)pub->Height : 512;
        if (pub) UnlockPubScreen(NULL, pub);

        win = OpenWindowTags(NULL,
            WA_Left,        (WORD)((sw - (IMG_W + 8)) / 2),
            WA_Top,         (WORD)((sh - (IMG_H + STATUS_H + 30)) / 2),
            WA_InnerWidth,  IMG_W,
            WA_InnerHeight, IMG_H + STATUS_H,
            WA_Title,       title,
            WA_CloseGadget, TRUE,
            WA_DragBar,     TRUE,
            WA_DepthGadget, TRUE,
            WA_Activate,    TRUE,
            WA_IDCMP,       IDCMP_CLOSEWINDOW,
            TAG_END);
    }

    if (!win) {
        printf("Cannot open window.\n");
        rc = 20;
        goto bye;
    }

    rp = win->RPort;

    SetAPen(rp, 0);
    RectFill(rp,
        win->BorderLeft,
        win->BorderTop,
        win->BorderLeft + IMG_W - 1,
        win->BorderTop + IMG_H + STATUS_H - 1);

    draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                IMG_W, "ZZFractal Zoom XACP - starting ARM tour...");

    for (frame = 0; frame < NUM_FRAMES; frame++) {
        LONG xmin, xmax, ymin, ymax;
        LONG timeout = 3000000L;
        UWORD st = 1;

        make_view(frame, &xmin, &xmax, &ymin, &ymax);

        xcmd->opcode          = OP_MANDELBROT;
        xcmd->framebuffer_ptr = XACP_OUT_OFFSET;
        xcmd->pitch_bytes     = IMG_W * 4;
        xcmd->width           = IMG_W;
        xcmd->height          = IMG_H;
        xcmd->max_iter        = MAXITER;
        xcmd->xmin_fp         = xmin;
        xcmd->xmax_fp         = xmax;
        xcmd->ymin_fp         = ymin;
        xcmd->ymax_fp         = ymax;

        snprintf(msg, sizeof(msg),
                 "ARM frame %ld/%ld...",
                 (LONG)(frame + 1), (LONG)NUM_FRAMES);
        draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                    IMG_W, msg);

        CurrentTime(&s0, &u0);
        ZZ_WR(board, REG_CMD, (UWORD)OP_MANDELBROT);

        do {
            st = ZZ_RD(board, REG_CMD);
            if (st == STATUS_DONE || st == STATUS_ERROR)
                break;
        } while (--timeout > 0);

        CurrentTime(&s1, &u1);

        if (st != STATUS_DONE) {
            snprintf(msg, sizeof(msg),
                     "ARM error/timeout frame %ld, status=0x%04x",
                     (LONG)frame, (unsigned)st);
            draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                        IMG_W, msg);
            break;
        }

        total_ms += elapsed_ms(s0, u0, s1, u1);

        WritePixelArray((APTR)(fb + XACP_OUT_OFFSET),
                        0, 0, IMG_W * 4,
                        rp,
                        win->BorderLeft,
                        win->BorderTop,
                        IMG_W, IMG_H,
                        RECTFMT_ARGB);

        snprintf(msg, sizeof(msg),
                 "ARM zoom tour: frame %ld/%ld | total %lu.%03lus",
                 (LONG)(frame + 1), (LONG)NUM_FRAMES,
                 total_ms / 1000UL, total_ms % 1000UL);
        draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                    IMG_W, msg);
    }

    if (frame == NUM_FRAMES) {
        ULONG avg = total_ms / NUM_FRAMES;
        snprintf(msg, sizeof(msg),
                 "DONE - %ld ARM frames | avg %lu.%03lus per frame",
                 (LONG)NUM_FRAMES, avg / 1000UL, avg % 1000UL);
        draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                    IMG_W, msg);
    }

    {
        BOOL done = FALSE;
        struct IntuiMessage *imsg;

        while (!done) {
            Wait(1L << win->UserPort->mp_SigBit);
            while ((imsg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                if (imsg->Class == IDCMP_CLOSEWINDOW)
                    done = TRUE;
                ReplyMsg((struct Message *)imsg);
            }
        }
    }

bye:
    if (win) CloseWindow(win);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);

    return rc;
}