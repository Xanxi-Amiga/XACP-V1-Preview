/*
 * ZZFractal_XACP_Offscreen.c
 * Démo stable : 68060 vs ARM ZZ9000 via XACP
 *
 * ARM calcule par bandes dans un buffer offscreen :
 *   fb + 0x04080000
 *
 * Le 68k affiche chaque bande avec WritePixelArray.
 *
 * Aucun accès ARM direct au framebuffer RTG visible.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <intuition/intuition.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <libraries/configvars.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/expansion.h>
#include <proto/cybergraphics.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#define ZZ9000_MANUF    0x6D6E
#define ZZ9000_PROD     0x04
#define MNT_FB_BASE     0x00010000UL

#define XACP_CMD_OFFSET 0x04000000UL
#define XACP_OUT_OFFSET 0x04080000UL

#define REG_CMD         0x64
#define OP_MANDELBROT   2
#define STATUS_DONE     2
#define STATUS_ERROR    3

#define IMG_W           640
#define HALF_W          320
#define IMG_H           480
#define STATUS_H        20
#define BAND_H          8
#define MAXITER         128

#define ZZ_WR(b,o,v) (*((volatile UWORD*)((UBYTE*)(b)+(o))) = (UWORD)(v))
#define ZZ_RD(b,o)   (*((volatile UWORD*)((UBYTE*)(b)+(o))))

typedef struct {
    ULONG opcode;
    ULONG input_offset;
    ULONG input_size;
    ULONG output_offset;
    ULONG output_max_size;
    ULONG param1;
    ULONG param2;
    ULONG param3;
    ULONG param4;
    ULONG result1;
    ULONG result2;
    ULONG status;
    ULONG error;
} XACP_Command;

/* 68060 Q12 */
#define CX_Q12      ((LONG)(-3046))
#define CY_Q12      ((LONG)(  540))
#define SCALE_Q12   ((LONG)(    2))

/* ARM Q16.16 */
#define ARM_XMIN    ((LONG)(-52891))
#define ARM_XMAX    ((LONG)(-44571))
#define ARM_YMIN    ((LONG)(  2401))
#define ARM_SCALE   ((LONG)(    26))

struct IntuitionBase *IntuitionBase = NULL;
struct GfxBase       *GfxBase       = NULL;
struct Library       *CyberGfxBase  = NULL;

static ULONG mandel_pixel_68k(LONG cr, LONG ci)
{
    LONG zr = 0, zi = 0;
    UWORD it;
    UBYTE t, r, g, b;

    for (it = 0; it < MAXITER; it++) {
        LONG zr2 = (zr * zr) >> 12;
        LONG zi2 = (zi * zi) >> 12;

        if (zr2 + zi2 > (LONG)(4 * 4096))
            break;

        zi = ((zr * zi) >> 11) + ci;
        zr = zr2 - zi2 + cr;
    }

    if (it >= MAXITER)
        return 0xFFFFFFFFUL;

    t = (UBYTE)((it & 31) << 3);

    switch ((it >> 5) & 3) {
        case 0: r=0;   g=0;     b=t;     break;
        case 1: r=0;   g=t;     b=255;   break;
        case 2: r=t;   g=255;   b=255-t; break;
        default:r=255; g=255-t; b=0;     break;
    }

    return 0xFF000000UL | ((ULONG)r << 16) | ((ULONG)g << 8) | b;
}

static void draw_status(struct RastPort *rp, WORD x, WORD y, const char *msg)
{
    SetAPen(rp, 0);
    RectFill(rp, x, y, x + IMG_W - 1, y + STATUS_H - 1);
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

int main(void)
{
    struct ConfigDev *cd = NULL;
    volatile UBYTE *board = NULL;
    volatile UBYTE *fb = NULL;
    struct Window *win = NULL;
    struct RastPort *rp = NULL;
    ULONG *linebuf = NULL;
    XACP_Command *xcmd = NULL;

    ULONG s0,u0,s1,u1;
    ULONG ms68k = 0, msARM = 0;
    WORD py, px;
    char buf[128];
    int rc = 0;

    static UBYTE title[] = "ZZFractal XACP Offscreen - Xanxi";

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

    linebuf = (ULONG *)AllocVec((ULONG)HALF_W * 4UL, MEMF_ANY | MEMF_CLEAR);
    if (!linebuf) {
        printf("Cannot allocate line buffer.\n");
        rc = 20;
        goto bye;
    }

    {
        struct Screen *pub = LockPubScreen(NULL);
        WORD sw = pub ? (WORD)pub->Width  : 640;
        WORD sh = pub ? (WORD)pub->Height : 512;
        if (pub) UnlockPubScreen(NULL, pub);

        win = OpenWindowTags(NULL,
            WA_Left,        (WORD)((sw - (IMG_W + 8)) / 2),
            WA_Top,         (WORD)((sh - (IMG_H + STATUS_H + 20)) / 2),
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

    SetAPen(rp, 1);
    Move(rp, win->BorderLeft + HALF_W, win->BorderTop);
    Draw(rp, win->BorderLeft + HALF_W, win->BorderTop + IMG_H - 1);

    Move(rp, win->BorderLeft + 4, win->BorderTop + rp->Font->tf_Baseline + 2);
    Text(rp, "68060", 5);

    Move(rp, win->BorderLeft + HALF_W + 4, win->BorderTop + rp->Font->tf_Baseline + 2);
    Text(rp, "ARM XACP", 8);

    draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H,
                "68060 rendering...");

    /* 68060 progressive rendering */
    CurrentTime(&s0, &u0);

    for (py = 0; py < IMG_H; py++) {
        LONG ci = CY_Q12 + (LONG)(py - IMG_H/2) * SCALE_Q12;

        for (px = 0; px < HALF_W; px++) {
            LONG cr = CX_Q12 + (LONG)(px - HALF_W/2) * SCALE_Q12;
            linebuf[px] = mandel_pixel_68k(cr, ci);
        }

        WritePixelArray(linebuf, 0, 0, HALF_W * 4,
                        rp,
                        win->BorderLeft,
                        win->BorderTop + py,
                        HALF_W, 1,
                        RECTFMT_ARGB);
    }

    CurrentTime(&s1, &u1);
    ms68k = elapsed_ms(s0,u0,s1,u1);

    snprintf(buf, sizeof(buf),
             "68060: %lu.%03lus | ARM XACP rendering...",
             ms68k/1000UL, ms68k%1000UL);
    draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H, buf);

    /* ARM progressive rendering by bands into offscreen buffer */
    CurrentTime(&s0, &u0);

    for (py = 0; py < IMG_H; py += BAND_H) {
        WORD band_h = BAND_H;
        LONG timeout = 3000000L;
        UWORD st = 1;

        if (py + band_h > IMG_H)
            band_h = IMG_H - py;

        xcmd->opcode          = OP_MANDELBROT;
        xcmd->output_offset   = XACP_OUT_OFFSET;  /* framebuffer_ptr */
        xcmd->output_max_size = HALF_W * 4;        /* pitch_bytes     */
        xcmd->param1          = HALF_W;             /* width           */
        xcmd->param2          = band_h;             /* height          */
        xcmd->param3          = MAXITER;            /* max_iter        */
        xcmd->param4          = ARM_XMIN;           /* xmin_fp         */
        xcmd->input_offset    = ARM_XMAX;           /* xmax_fp         */
        xcmd->input_size      = ARM_YMIN + (LONG)py * ARM_SCALE;  /* ymin_fp */
        xcmd->result1         = ARM_YMIN + (LONG)(py + band_h) * ARM_SCALE; /* ymax_fp */

        ZZ_WR(board, REG_CMD, (UWORD)OP_MANDELBROT);

        do {
            st = ZZ_RD(board, REG_CMD);
            if (st == STATUS_DONE || st == STATUS_ERROR)
                break;
        } while (--timeout > 0);

        if (st != STATUS_DONE) {
            snprintf(buf, sizeof(buf),
                     "ARM error/timeout at line %ld, status=0x%04x",
                     (LONG)py, (unsigned)st);
            draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H, buf);
            break;
        }

        WritePixelArray((APTR)(fb + XACP_OUT_OFFSET),
                        0, 0, HALF_W * 4,
                        rp,
                        win->BorderLeft + HALF_W,
                        win->BorderTop + py,
                        HALF_W, band_h,
                        RECTFMT_ARGB);
    }

    CurrentTime(&s1, &u1);
    msARM = elapsed_ms(s0,u0,s1,u1);

    if (msARM > 0) {
        snprintf(buf, sizeof(buf),
                 "68060: %lu.%03lus | ARM XACP: %lu.%03lus | x%lu",
                 ms68k/1000UL, ms68k%1000UL,
                 msARM/1000UL, msARM%1000UL,
                 ms68k/msARM);
    } else {
        snprintf(buf, sizeof(buf),
                 "68060: %lu.%03lus | ARM XACP: <1ms",
                 ms68k/1000UL, ms68k%1000UL);
    }

    draw_status(rp, win->BorderLeft, win->BorderTop + IMG_H, buf);

    /* wait close */
    {
        BOOL done = FALSE;
        struct IntuiMessage *msg;

        while (!done) {
            Wait(1L << win->UserPort->mp_SigBit);

            while ((msg = (struct IntuiMessage *)GetMsg(win->UserPort))) {
                if (msg->Class == IDCMP_CLOSEWINDOW)
                    done = TRUE;
                ReplyMsg((struct Message *)msg);
            }
        }
    }

bye:
    if (win) CloseWindow(win);
    if (linebuf) FreeVec(linebuf);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
    if (GfxBase) CloseLibrary((struct Library *)GfxBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);

    return rc;
}