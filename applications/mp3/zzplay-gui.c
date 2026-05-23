/*
 * ZZPlayGUI.c — ZZ9000 XACP MP3 Player GUI
 * Version 1.0 / XX16
 *
 * GUI : Intuition/GadTools AmigaOS 3.1
 * Architecturé sur g13MULTI6-256-8.c (MULTIPLIER=8, ring circulaire)
 *
 * Build (two-step required — one-step fails with this toolchain):
 *   m68k-amigaos-gcc -O2 -noixemul -m68020 -c -o zzplay-gui.o zzplay-gui.c
 *   m68k-amigaos-gcc      -noixemul -m68020 -o ZZPlayGUI   zzplay-gui.o -lamiga
 *
 * Usage :
 *   zzplay-gui [fichier1.mp3 fichier2.mp3 ...]
 *   Bouton OUV : sélection ASL multi-fichiers
 *   Espace : play/pause — Echap : stop — Flèches : prev/next
 */

#define __USE_SYSBASE

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <intuition/gadgetclass.h>   /* GA_RelVerify */
#include <libraries/gadtools.h>
#include <libraries/asl.h>
#include <libraries/configvars.h>
#include <devices/ahi.h>
#include <exec/lists.h>
#include <graphics/rastport.h>
#include <graphics/text.h>

#include <workbench/startup.h>   /* struct WBArg pour ASL multi-select */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>
#include <proto/asl.h>
#include <proto/expansion.h>
#include <proto/graphics.h>
/* proto/ahi.h non inclus : AHI utilisé comme device, pas comme library */

#include <string.h>
#include <stdio.h>

/* ================================================================
 * Bases des librairies — déclarées par proto/*.h (bebbo NDK)
 * On déclare seulement GadToolsBase et AslBase qui sont
 * struct Library* simples et pas toujours dans le NDK de base.
 * IntuitionBase et GfxBase sont déclarés automatiquement.
 * ================================================================ */
struct Library *GadToolsBase = NULL;
struct Library *AslBase      = NULL;

/* ================================================================
 * Constantes XACP (doivent correspondre exactement au firmware)
 * ================================================================ */
#define ZZ9000_MANUF        0x6D6Eu
#define ZZ9000_PROD_AX      0x0Au
#define ZZ9000_PROD         0x04u

#define MNT_FB_BASE         0x00010000UL
#define REG_CMD             0x64

#define XACP_STREAM_OFFSET  0x04002000UL
#define XACP_MP3_OFFSET     0x04100000UL
#define XACP_PCM_OFFSET     0x04200000UL

#define MP3_RING_SIZE       (512UL  * 1024UL)
#define PCM_RING_SIZE       (1024UL * 1024UL)

#define OP_STREAM_OPEN      4
#define OP_STREAM_CLOSE     5
#define STREAM_DECODE_DONE  2

#define MP3_PUSH_SIZE       (256UL * 1024UL)

#define MULTIPLIER          8                    /* ~160 ms/buffer @ 44100 stereo */
#define AHI_BUFSIZE         65536UL              /* doit être >= g_chunk max       */

#define BE32(x) (x)   /* 68k = big-endian natif */

#define ZZ_WR(b,o,v) (*((volatile UWORD *)((UBYTE *)(b) + (o))) = (UWORD)(v))
#define ZZ_RD(b,o)   (*((volatile UWORD *)((UBYTE *)(b) + (o))))

typedef struct {
    volatile ULONG mp3_base, mp3_size, mp3_write, mp3_read;
    volatile ULONG mp3_need_refill, mp3_eof;
    volatile ULONG pcm_base, pcm_size, pcm_write, pcm_read;
    volatile ULONG sample_rate, channels;
    volatile ULONG status, error;
    volatile ULONG underrun_count, frames_decoded, flags;
} XACP_StreamControl;

/* ================================================================
 * Gadgets IDs
 * ================================================================ */
#define GAD_PREV     1
#define GAD_REW      2
#define GAD_STOP     3
#define GAD_PLAY     4
#define GAD_PAUSE    5
#define GAD_FF       6
#define GAD_NEXT     7
#define GAD_OPEN     8
#define GAD_PLS      9
#define GAD_LOOP     10
#define GAD_PROGRESS 11
#define GAD_COUNT    12

/* Labels (kept for reference — icons used instead) */
static const char *BTN_LABELS[GAD_COUNT] = {
    NULL, "|<", "<<", "[]", "|>", "||", ">>", ">|", "OP", "PLS", "RPT", NULL
};

/* ================================================================
 * Transport button bitmap icons — 16×8 pixels, 1 bitplane
 * bit15 = leftmost pixel (column 0)
 * Rendered with DrawImage after GT_RefreshWindow and after GADGETUP.
 * pen0 (background) matches GadTools button face on standard WB.
 * ================================================================ */
#define BTN_ICON_W  16
#define BTN_ICON_H   8

/* 1: PREV |<  — left-pointing triangle + right vertical bar */
static const UWORD g_idata_prev[BTN_ICON_H] = {
    0x0000, 0x0408, 0x0C08, 0x1C08, 0x1C08, 0x0C08, 0x0408, 0x0000
};
/* 2: REW <<   — two left-pointing triangles */
static const UWORD g_idata_rew[BTN_ICON_H] = {
    0x0000, 0x0440, 0x0CC0, 0x1DC0, 0x1DC0, 0x0CC0, 0x0440, 0x0000
};
/* 3: STOP []  — solid square */
static const UWORD g_idata_stop[BTN_ICON_H] = {
    0x0000, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0000
};
/* 4: PLAY |>  — right-pointing triangle */
static const UWORD g_idata_play[BTN_ICON_H] = {
    0x1000, 0x1800, 0x1C00, 0x1E00, 0x1C00, 0x1800, 0x1000, 0x0000
};
/* 5: PAUSE || — two vertical bars */
static const UWORD g_idata_pause[BTN_ICON_H] = {
    0x0000, 0x1B00, 0x1B00, 0x1B00, 0x1B00, 0x1B00, 0x1B00, 0x0000
};
/* 6: FF >>    — two right-pointing triangles */
static const UWORD g_idata_ff[BTN_ICON_H] = {
    0x0000, 0x0220, 0x0330, 0x03B8, 0x03B8, 0x0330, 0x0220, 0x0000
};
/* 7: NEXT >|  — right-pointing triangle + left vertical bar */
static const UWORD g_idata_next[BTN_ICON_H] = {
    0x0000, 0x1080, 0x1880, 0x1C80, 0x1C80, 0x1880, 0x1080, 0x0000
};
/* 8: OPEN     — folder outline */
static const UWORD g_idata_open[BTN_ICON_H] = {
    0x0000, 0x1800, 0x3FC0, 0x2040, 0x2040, 0x2040, 0x3FC0, 0x0000
};
/* 9: PLS      — three horizontal lines (playlist) */
static const UWORD g_idata_pls[BTN_ICON_H] = {
    0x0000, 0x3FE0, 0x0000, 0x3FE0, 0x0000, 0x3FE0, 0x0000, 0x0000
};
/* 10: LOOP    — rectangular loop with arrowheads (repeat mode) */
static const UWORD g_idata_loop[BTN_ICON_H] = {
    0x0000, 0x1FC0, 0x2020, 0x2070, 0x7020, 0x1020, 0x0FC0, 0x0000
};

static struct Image g_btn_img[10];  /* one per transport button */

/* Echelle de la barre de progression */
#define PROGRESS_MAX 1000UL

/* Largeur fenêtre */
#define WIN_WIDTH    320

/* Playlist */
#define MAX_PLAYLIST  256
#define PATH_BUF_SIZE 512   /* taille fixe de chaque entrée playlist */

/* ================================================================
 * Etat global du player
 * ================================================================ */
typedef enum { STATE_STOPPED = 0, STATE_PLAYING, STATE_PAUSED } PlayerState;

/* Fenêtre et gadgets */
static struct Window   *g_win   = NULL;
static struct Screen   *g_scr   = NULL;
static APTR             g_vi    = NULL;
static struct Gadget   *g_glist = NULL;
static struct Gadget   *g_gad[GAD_COUNT];

/* Coordonnées des lignes de texte (pixels fenêtre absolus) */
static WORD g_tx, g_ty[3], g_th;   /* x, y[3 lignes], hauteur ligne */
static WORD g_iw;                   /* largeur interne */

/* ZZ9000 */
static UBYTE *g_board = NULL;
static UBYTE *g_fb    = NULL;

/* Playlist */
static char  *g_playlist[MAX_PLAYLIST];
static int    g_pcount = 0;
static int    g_cur    = -1;

/* Playback */
static PlayerState      g_state  = STATE_STOPPED;
static UBYTE           *g_mp3src = NULL;
static LONG             g_mp3sz  = 0;
static ULONG            g_srcpos = 0;
static ULONG            g_wptr   = 0;
static ULONG            g_prd    = 0;
static ULONG            g_sr     = 44100, g_ch = 2;
static ULONG            g_chunk  = 0;
static ULONG            g_tfest  = 0;   /* total frames estimé */
static XACP_StreamControl *g_sc  = NULL;
static UBYTE           *g_mring  = NULL;
static UBYTE           *g_pring  = NULL;

/* AHI */
static struct AHIRequest *g_req[2]    = {NULL, NULL};
static struct MsgPort    *g_ahiport   = NULL;
static BOOL               g_act[2]   = {FALSE, FALSE};
static int                g_acur = 0, g_anext = 1;
static BOOL               g_eofsent  = FALSE;

/* Buffers PCM locaux (Fast RAM) */
static UBYTE g_buf0[AHI_BUFSIZE];
static UBYTE g_buf1[AHI_BUFSIZE];

/* Display */
static char  g_dispname[48];          /* nom de fichier tronqué */
static BOOL  g_quit = FALSE;
static BOOL  g_loop = FALSE;          /* repeat/loop mode */

/* Display cache — skip redraw when content unchanged */
static char  g_row_cache[3][80];      /* last drawn string per row */
static ULONG g_last_slider = 0xFFFFFFFFUL; /* force first draw */

/* Bytes réellement consommés par le décodeur ARM (pour bitrate/durée stables) */
static ULONG g_mp3rd_prev  = 0;   /* valeur précédente de sc->mp3_read  */
static ULONG g_mp3_decoded = 0;   /* total accumulé depuis stream_open  */

/* Fenêtre playlist */
static struct Window *g_plswin   = NULL;
static struct Gadget *g_plsglist = NULL;
static struct Gadget *g_plsgad   = NULL;

/* Exec List pour le LISTVIEW_KIND (nœuds persistants) */
static struct List g_pls_execlist;
static struct Node g_pls_nodes[MAX_PLAYLIST];

/* ================================================================
 * Helpers ring buffer
 * ================================================================ */
static ULONG ring_avail(ULONG w, ULONG r, ULONG sz) {
    return (w >= r) ? (w - r) : (sz - r + w);
}
static ULONG ring_free(ULONG w, ULONG r, ULONG sz) {
    return (r + sz - w - 1) % sz;
}
static void ring_push(UBYTE *ring, ULONG rsz, ULONG *wp,
                      const UBYTE *src, ULONG len)
{
    ULONG w   = *wp;
    ULONG ste = rsz - w;
    if (len <= ste) {
        CopyMem((APTR)src, ring + w, len);
    } else {
        CopyMem((APTR)src,       ring + w, ste);
        CopyMem((APTR)(src+ste), ring,     len - ste);
    }
    *wp = (w + len) % rsz;
}
static void ring_copy_out(UBYTE *ring, ULONG rsz,
                           ULONG rd, UBYTE *dst, ULONG len)
{
    ULONG ste = rsz - rd;
    if (len <= ste) {
        CopyMem((APTR)(ring + rd), dst, len);
    } else {
        CopyMem((APTR)(ring + rd), dst,       ste);
        CopyMem((APTR)ring,        dst + ste, len - ste);
    }
}

/* ================================================================
 * Affichage — texte direct dans RastPort
 * ================================================================ */
static void draw_row(int row, const char *str)
{
    struct RastPort *rp;
    WORD x, y;
    const char *s = (str && str[0]) ? str : "";

    /* Skip expensive blit if content hasn't changed */
    if (strcmp(g_row_cache[row], s) == 0) return;
    strncpy(g_row_cache[row], s, 79);
    g_row_cache[row][79] = '\0';

    rp = g_win->RPort;
    x  = g_tx;
    y  = g_ty[row];

    SetAPen(rp, 0);
    RectFill(rp, x, y, x + g_iw - 1, y + g_th - 1);
    if (s[0]) {
        SetAPen(rp, 1);
        SetDrMd(rp, JAM1);
        Move(rp, x + 2, y + rp->Font->tf_Baseline);
        Text(rp, (STRPTR)s, (LONG)strlen(s));
    }
}

static void update_display(void)
{
    char buf[80];
    ULONG frames = 0, elapsed_ms = 0, total_ms = 0;
    ULONG sr = g_sr, ch = g_ch, bitrate = 0;
    ULONG em, es, tm, ts;

    if (g_state != STATE_STOPPED && g_sc) {
        CacheClearU();
        frames = BE32(g_sc->frames_decoded);
        sr     = BE32(g_sc->sample_rate); if (!sr) sr = g_sr;
        ch     = BE32(g_sc->channels);    if (!ch || ch > 2) ch = g_ch;

        /* Temps écoulé : 1152 samples/frame MPEG-1 L3 */
        if (sr > 0)
            elapsed_ms = (frames * 1152UL / sr) * 1000UL;

        if (g_mp3_decoded > 0 && frames > 0) {
            /* bytes/frame moyen = décodés réels → stable pour CBR */
            ULONG bpf = g_mp3_decoded / frames;   /* bytes per frame */

            /* Bitrate kbps = bits/ms */
            if (bpf > 0 && elapsed_ms > 0)
                bitrate = (g_mp3_decoded * 8UL) / elapsed_ms;

            /* Durée totale : sans multiplication g_mp3sz*frames (overflow) */
            if (bpf > 0)
                g_tfest = (ULONG)g_mp3sz / bpf;
        }

        if (g_tfest > 0 && sr > 0)
            total_ms = (g_tfest * 1152UL / sr) * 1000UL;
    }

    em = elapsed_ms / 60000UL; es = (elapsed_ms % 60000UL) / 1000UL;
    tm = total_ms   / 60000UL; ts = (total_ms   % 60000UL) / 1000UL;

    /* Ligne 0 : nom de fichier */
    draw_row(0, g_dispname);

    /* Ligne 1 : temps / durée — fréquence — canaux */
    sprintf(buf, "%lu:%02lu / %lu:%02lu  %luHz  %s",
            em, es, tm, ts, sr, (ch == 2) ? "Stereo" : "Mono");
    draw_row(1, buf);

    /* Ligne 2 : bitrate moyen — frames — underruns */
    if (g_state != STATE_STOPPED && g_sc) {
        sprintf(buf, "%lu kbps  %luHz  %s",
                bitrate, sr,
                (g_state == STATE_PAUSED) ? "[PAUSED]" : "");
    } else {
        sprintf(buf, "Stopped");
    }
    draw_row(2, buf);

    /* Barre de progression — dessinée DIRECTEMENT via RastPort/GfxBase.
     * GT_SetGadgetAttrs bloquerait sur le verrou Intuition pendant le
     * tracking du menu WB (clic droit) et couperait l'audio.
     * RectFill n'utilise que le verrou de la layer de notre fenêtre :
     * indépendant du menu, jamais bloquant pour nous. */
    if (g_gad[GAD_PROGRESS] && g_tfest > 0) {
        ULONG level = (frames < g_tfest) ? (frames * PROGRESS_MAX / g_tfest)
                                          : PROGRESS_MAX;
        if (level >= g_last_slider + 5 || level + 5 <= g_last_slider
                || g_last_slider == 0xFFFFFFFFUL) {
            struct RastPort *rp = g_win->RPort;
            WORD gx = g_gad[GAD_PROGRESS]->LeftEdge + 2;
            WORD gy = g_gad[GAD_PROGRESS]->TopEdge  + 2;
            WORD gw = g_gad[GAD_PROGRESS]->Width  - 4;
            WORD gh = g_gad[GAD_PROGRESS]->Height - 4;
            WORD fw = (gw > 0) ? (WORD)((ULONG)gw * level / PROGRESS_MAX) : 0;
            SetAPen(rp, 1);
            if (fw > 0)
                RectFill(rp, gx, gy, gx + fw - 1, gy + gh - 1);
            SetAPen(rp, 0);
            if (fw < gw)
                RectFill(rp, gx + fw, gy, gx + gw - 1, gy + gh - 1);
            g_last_slider = level;
        }
    }
}

/* ================================================================
 * AHI — envoi d'un chunk PCM
 * ================================================================ */
static void send_ahi_chunk(int slot, UBYTE *buf, ULONG len)
{
    ULONG ahi_type = (g_ch == 2) ? AHIST_S16S : AHIST_M16S;
    g_req[slot]->ahir_Std.io_Command = CMD_WRITE;
    g_req[slot]->ahir_Std.io_Data    = buf;
    g_req[slot]->ahir_Std.io_Length  = len;
    g_req[slot]->ahir_Frequency      = g_sr;
    g_req[slot]->ahir_Type           = ahi_type;
    g_req[slot]->ahir_Volume         = 0x10000L;
    g_req[slot]->ahir_Position       = 0x8000L;
    /* Chain behind the other slot if it is active — gapless double-buffering */
    g_req[slot]->ahir_Link           = g_act[slot ^ 1] ? g_req[slot ^ 1] : NULL;
    SendIO((struct IORequest *)g_req[slot]);
    g_act[slot] = TRUE;
}

/* ================================================================
 * AHI — arrêt propre de tous les slots
 * ================================================================ */
static void ahi_stop_all(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_req[i] && g_act[i]) {
            if (!CheckIO((struct IORequest *)g_req[i]))
                AbortIO((struct IORequest *)g_req[i]);
            WaitIO((struct IORequest *)g_req[i]);
            g_act[i] = FALSE;
        }
    }
}

/* ================================================================
 * Nom de fichier — extrait le basename sans extension
 * ================================================================ */
static void set_dispname(const char *path)
{
    const char *n = path;
    const char *p = path;
    int len;
    while (*p) { if (*p == '/' || *p == ':') n = p + 1; p++; }
    len = (int)strlen(n);
    if (len >= (int)sizeof(g_dispname))
        len = (int)sizeof(g_dispname) - 1;
    memcpy(g_dispname, n, (size_t)len);
    g_dispname[len] = '\0';
    if (len > 4) {
        char *ext = g_dispname + len - 4;
        if (ext[0] == '.' &&
            (ext[1] == 'm' || ext[1] == 'M') &&
            (ext[2] == 'p' || ext[2] == 'P') &&
            (ext[3] == '3'))
            *ext = '\0';
    }
}

/* ================================================================
 * stream_open — lance le streaming XACP depuis src_pos
 *   Suppose g_mp3src alloué et g_board/g_fb valides.
 *   Retourne TRUE si le streaming a démarré avec succès.
 * ================================================================ */
static BOOL stream_open(ULONG start_pos)
{
    ULONG prefill, avail, w, timeout;
    UBYTE *buf0;

    g_sc    = (XACP_StreamControl *)(g_fb + XACP_STREAM_OFFSET);
    g_mring = g_fb + XACP_MP3_OFFSET;
    g_pring = g_fb + XACP_PCM_OFFSET;

    /* Init structure de contrôle */
    memset((void *)g_sc, 0, sizeof(XACP_StreamControl));
    g_sc->mp3_base = BE32(XACP_MP3_OFFSET);
    g_sc->mp3_size = BE32(MP3_RING_SIZE);
    g_sc->pcm_base = BE32(XACP_PCM_OFFSET);
    g_sc->pcm_size = BE32(PCM_RING_SIZE);

    /* Prefill ring MP3 à 75% */
    g_wptr        = 0;
    g_srcpos      = start_pos;
    g_prd         = 0;
    g_eofsent     = FALSE;
    g_mp3rd_prev  = 0;
    g_mp3_decoded = 0;

    /* Invalidate display caches so new track info is drawn immediately */
    g_row_cache[0][0] = '\0';
    g_row_cache[1][0] = '\0';
    g_row_cache[2][0] = '\0';
    g_last_slider     = 0xFFFFFFFFUL;

    prefill = (MP3_RING_SIZE * 3UL) / 4UL;
    if ((ULONG)g_mp3sz - start_pos < prefill)
        prefill = (ULONG)g_mp3sz - start_pos;

    ring_push(g_mring, MP3_RING_SIZE, &g_wptr,
              g_mp3src + g_srcpos, prefill);
    g_srcpos += prefill;
    g_sc->mp3_write = BE32(g_wptr);

    ZZ_WR(g_board, REG_CMD, OP_STREAM_OPEN);

    /* Attente sample_rate (ARM décode la première frame) */
    { volatile int d = 20000; while (d--); }
    timeout = 0;
    while (g_sc->sample_rate == 0) {
        struct IntuiMessage *msg;
        volatile int d = 500; while (d--);
        if (++timeout > 2000) return FALSE;   /* ~1s timeout */
        /* Répondre aux events pendant l'attente */
        while ((msg = (struct IntuiMessage *)GetMsg(g_win->UserPort))) {
            if (msg->Class == IDCMP_CLOSEWINDOW) g_quit = TRUE;
            ReplyMsg((struct Message *)msg);
        }
        if (g_quit) return FALSE;
    }

    g_sr = BE32(g_sc->sample_rate);
    g_ch = BE32(g_sc->channels);
    if (!g_sr) g_sr = 44100;
    if (!g_ch || g_ch > 2) g_ch = 2;

    g_chunk = (g_sr / 50UL) * g_ch * 2UL * MULTIPLIER;
    if (g_chunk > AHI_BUFSIZE) g_chunk = AHI_BUFSIZE;

    /* Estimation initiale de la durée totale
     * ~417 bytes/frame à 128kbps 44100Hz */
    g_tfest = (ULONG)g_mp3sz / 417UL;

    /* Mise à jour barre progression max */
    if (g_gad[GAD_PROGRESS])
        GT_SetGadgetAttrs(g_gad[GAD_PROGRESS], g_win, NULL,
                          GTSL_Max, PROGRESS_MAX,
                          GTSL_Level, 0UL,
                          TAG_END);

    /* Attendre le premier chunk PCM */
    CacheClearU();
    w     = BE32(g_sc->pcm_write);
    avail = ring_avail(w, g_prd, PCM_RING_SIZE);
    timeout = 0;
    while (avail < g_chunk) {
        struct IntuiMessage *msg;
        Delay(1);
        CacheClearU();
        w     = BE32(g_sc->pcm_write);
        avail = ring_avail(w, g_prd, PCM_RING_SIZE);
        if (++timeout > 300) return FALSE;
        while ((msg = (struct IntuiMessage *)GetMsg(g_win->UserPort))) {
            if (msg->Class == IDCMP_CLOSEWINDOW) g_quit = TRUE;
            ReplyMsg((struct Message *)msg);
        }
        if (g_quit) return FALSE;
    }

    /* Envoyer le premier buffer AHI */
    buf0 = g_buf0;
    ring_copy_out(g_pring, PCM_RING_SIZE, g_prd, buf0, g_chunk);
    g_prd = (g_prd + g_chunk) % PCM_RING_SIZE;
    g_sc->pcm_read = BE32(g_prd);

    g_acur  = 0;
    g_anext = 1;
    send_ahi_chunk(0, buf0, g_chunk);   /* req[0] démarre (ahir_Link=NULL).
     * player_tick remplira et enverra req[1] (ahir_Link=req[0]) AVANT le
     * premier WaitIO — premier enchaînement déjà sans coupure. */

    g_state = STATE_PLAYING;
    update_display();
    return TRUE;
}

/* ================================================================
 * stream_close — ferme le streaming XACP et stoppe AHI
 * ================================================================ */
static void stream_close(void)
{
    ahi_stop_all();
    if (g_sc) {
        g_sc->mp3_eof = BE32(1);
        ZZ_WR(g_board, REG_CMD, OP_STREAM_CLOSE);
        Delay(2);
    }
    g_state = STATE_STOPPED;
}

/* ================================================================
 * player_stop — arrêt complet + libération fichier MP3
 * ================================================================ */
static void player_stop(void)
{
    stream_close();
    if (g_mp3src) {
        FreeMem(g_mp3src, g_mp3sz);
        g_mp3src = NULL;
    }
    g_mp3sz = 0;
    if (g_gad[GAD_PROGRESS])
        GT_SetGadgetAttrs(g_gad[GAD_PROGRESS], g_win, NULL,
                          GTSL_Level, 0UL, TAG_END);
    strcpy(g_dispname, "Stopped");
    draw_row(0, g_dispname);
    draw_row(1, "");
    draw_row(2, "");
}

/* ================================================================
 * player_start — charge un fichier et démarre la lecture
 * ================================================================ */
static BOOL player_start(int track)
{
    const char *path;
    BPTR fh;

    if (track < 0 || track >= g_pcount) return FALSE;
    player_stop();   /* libère l'éventuel fichier précédent */

    path = g_playlist[track];
    g_cur = track;
    set_dispname(path);

    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        strcpy(g_dispname, "Open error");
        draw_row(0, g_dispname);
        return FALSE;
    }
    Seek(fh, 0, OFFSET_END);
    g_mp3sz = Seek(fh, 0, OFFSET_BEGINNING);
    if (g_mp3sz <= 0) { Close(fh); return FALSE; }

    g_mp3src = (UBYTE *)AllocMem((ULONG)g_mp3sz, MEMF_ANY);
    if (!g_mp3src) { Close(fh); return FALSE; }
    Read(fh, g_mp3src, g_mp3sz);
    Close(fh);

    return stream_open(0);
}

/* ================================================================
 * player_seek — cherche dans le fichier courant (fraction 0-PROGRESS_MAX)
 * ================================================================ */
static void player_seek(ULONG level)
{
    ULONG pos;
    if (!g_mp3src || g_mp3sz <= 0) return;
    /* Eviter overflow : (g_mp3sz >> 10) * level / (PROGRESS_MAX >> 10) */
    pos = ((ULONG)g_mp3sz / PROGRESS_MAX) * level
        + ((ULONG)g_mp3sz % PROGRESS_MAX) * level / PROGRESS_MAX;
    if (pos >= (ULONG)g_mp3sz) pos = (ULONG)g_mp3sz - 1;
    stream_close();
    stream_open(pos);
}

/* ================================================================
 * player_tick — une itération de la boucle de lecture
 *   Retourne FALSE si le morceau est terminé.
 * ================================================================ */
static BOOL player_tick(void)
{
    UBYTE *buf;
    ULONG  pcm_w, avail, chunk;
    BOOL   done;
    ULONG  timeout;

    if (g_state != STATE_PLAYING) return TRUE;

    /* Lecture fraîche des champs ARM avant le refill */
    CacheClearU();
    done = (BE32(g_sc->status) == STREAM_DECODE_DONE);

    /* Refill ring MP3 si nécessaire */
    if (!g_eofsent && BE32(g_sc->mp3_need_refill)) {
        ULONG rp    = BE32(g_sc->mp3_read);
        ULONG mfree = ring_free(g_wptr, rp, MP3_RING_SIZE);
        ULONG left  = (ULONG)g_mp3sz - g_srcpos;
        ULONG push  = MP3_PUSH_SIZE;
        if (push > left)  push = left;
        if (push > mfree) push = mfree;
        if (push > 0) {
            ring_push(g_mring, MP3_RING_SIZE, &g_wptr,
                      g_mp3src + g_srcpos, push);
            g_srcpos += push;
            g_sc->mp3_write     = BE32(g_wptr);
            g_sc->mp3_need_refill = 0;
        }
    }
    if (!g_eofsent && g_srcpos >= (ULONG)g_mp3sz) {
        g_sc->mp3_eof = BE32(1);
        g_eofsent = TRUE;
    }

    /* Lire disponibilité PCM */
    CacheClearU();
    pcm_w = BE32(g_sc->pcm_write);
    avail = ring_avail(pcm_w, g_prd, PCM_RING_SIZE);

    /* Fin de morceau : plus de PCM, drainer g_acur avant d'arrêter */
    if (done && avail == 0) {
        if (g_act[g_acur]) {
            WaitIO((struct IORequest *)g_req[g_acur]);
            g_act[g_acur] = FALSE;
        }
        g_state = STATE_STOPPED;
        return FALSE;
    }

    chunk = g_chunk;
    if (done && avail < g_chunk) {
        chunk = avail & ~3UL;
        if (chunk == 0) {
            if (g_act[g_acur]) {
                WaitIO((struct IORequest *)g_req[g_acur]);
                g_act[g_acur] = FALSE;
            }
            g_state = STATE_STOPPED;
            return FALSE;
        }
    }

    /* Attendre assez de PCM (underrun temporaire) */
    timeout = 0;
    while (avail < chunk) {
        if (CheckSignal(SIGBREAKF_CTRL_C)) { g_quit = TRUE; return FALSE; }
        if (done && avail == 0) break;
        Delay(1);
        CacheClearU();
        pcm_w = BE32(g_sc->pcm_write);
        avail = ring_avail(pcm_w, g_prd, PCM_RING_SIZE);
        if (++timeout > 200) break;   /* ~4s : ARM en erreur ? */
    }

    /* ---------------------------------------------------------------
     * ORDRE CRITIQUE : remplir g_anext et l'ENVOYER AVANT WaitIO.
     *
     * send_ahi_chunk(g_anext) pose ahir_Link = g_req[g_acur] (en lecture).
     * AHI enchaîne g_anext → g_acur à niveau interruption, indépendamment
     * de notre tâche.  Tout ce qui se passe après le SendIO (menu WB,
     * GT_SetGadgetAttrs, etc.) ne peut plus créer de coupure.
     * --------------------------------------------------------------- */
    buf = (g_anext == 0) ? g_buf0 : g_buf1;
    ring_copy_out(g_pring, PCM_RING_SIZE, g_prd, buf, chunk);
    g_prd = (g_prd + chunk) % PCM_RING_SIZE;
    g_sc->pcm_read = BE32(g_prd);

    send_ahi_chunk(g_anext, buf, chunk);       /* ← AVANT WaitIO */

    /* Attendre g_acur — g_anext est déjà en file, aucun trou possible */
    if (g_act[g_acur]) {
        WaitIO((struct IORequest *)g_req[g_acur]);
        g_act[g_acur] = FALSE;
    }

    /* Cumuler octets MP3 (données fraîches post-WaitIO) */
    CacheClearU();
    {
        ULONG mp3rd = BE32(g_sc->mp3_read);
        ULONG delta = (mp3rd >= g_mp3rd_prev)
                    ? (mp3rd - g_mp3rd_prev)
                    : (MP3_RING_SIZE - g_mp3rd_prev + mp3rd);
        g_mp3_decoded += delta;
        g_mp3rd_prev   = mp3rd;
    }

    /* Swap : g_acur ← ancien g_anext (maintenant en lecture),
     *        g_anext ← ancien g_acur (slot libéré pour le prochain tick) */
    { int t = g_acur; g_acur = g_anext; g_anext = t; }

    /* Affichage — peut être lent, g_anext sera rempli au prochain tick */
    update_display();
    return TRUE;
}

/* ================================================================
 * Icon rendering — defined here, after globals, before callers
 * ================================================================ */

/* Populate g_btn_img[] — call once before any DrawImage */
static void init_btn_images(void)
{
    static const UWORD * const src[10] = {
        g_idata_prev, g_idata_rew,  g_idata_stop,
        g_idata_play, g_idata_pause, g_idata_ff,
        g_idata_next, g_idata_open, g_idata_pls, g_idata_loop
    };
    int i;
    for (i = 0; i < 10; i++) {
        g_btn_img[i].LeftEdge   = 0;
        g_btn_img[i].TopEdge    = 0;
        g_btn_img[i].Width      = BTN_ICON_W;
        g_btn_img[i].Height     = BTN_ICON_H;
        g_btn_img[i].Depth      = 1;
        g_btn_img[i].ImageData  = (UWORD *)src[i];
        g_btn_img[i].PlanePick  = 0x01;
        g_btn_img[i].PlaneOnOff = 0x00;
        g_btn_img[i].NextImage  = NULL;
    }
}

/* Draw icon bitmaps centred over each transport button.
 * Called after GT_RefreshWindow and after each button GADGETUP.
 * Loop button gets a 3×3 dot indicator when active. */
static void draw_icons(struct Window *win)
{
    int i;
    if (!win) return;
    for (i = 1; i <= 10; i++) {
        if (g_gad[i]) {
            WORD ix = g_gad[i]->LeftEdge +
                      (WORD)((g_gad[i]->Width  - BTN_ICON_W) / 2);
            WORD iy = g_gad[i]->TopEdge  +
                      (WORD)((g_gad[i]->Height - BTN_ICON_H) / 2);
            DrawImage(win->RPort, &g_btn_img[i - 1], ix, iy);
        }
    }
    /* Loop active indicator: 3×3 dot in bottom-right of loop button face */
    if (g_gad[GAD_LOOP]) {
        WORD bx = g_gad[GAD_LOOP]->LeftEdge + g_gad[GAD_LOOP]->Width  - 5;
        WORD by = g_gad[GAD_LOOP]->TopEdge  + g_gad[GAD_LOOP]->Height - 5;
        SetAPen(win->RPort, g_loop ? 1 : 0);   /* 1=black=active, 0=white=erase */
        RectFill(win->RPort, bx, by, bx + 2, by + 2);
    }
}

/* ================================================================
 * handle_idcmp — traitement des événements Intuition
 * ================================================================ */
/* Forward declaration (définie après handle_idcmp) */
static void open_playlist_window(void);

static void handle_idcmp(void)
{
    struct IntuiMessage *msg;

    while ((msg = (struct IntuiMessage *)GetMsg(g_win->UserPort))) {
        ULONG  class = msg->Class;
        UWORD  code  = msg->Code;
        struct Gadget *gad = (struct Gadget *)msg->IAddress;
        ReplyMsg((struct Message *)msg);

        if (class == IDCMP_CLOSEWINDOW) {
            g_quit = TRUE;
            return;
        }

        if (class == IDCMP_GADGETUP && gad) {
            switch (gad->GadgetID) {

            case GAD_PLAY:
                if (g_state == STATE_PAUSED) {
                    /* Reprendre : renvoyer le buffer courant */
                    UBYTE *buf = (g_acur == 0) ? g_buf0 : g_buf1;
                    g_state = STATE_PLAYING;
                    send_ahi_chunk(g_acur, buf, g_chunk);
                } else if (g_state == STATE_STOPPED && g_pcount > 0) {
                    int t = (g_cur >= 0 && g_cur < g_pcount) ? g_cur : 0;
                    player_start(t);
                }
                break;

            case GAD_PAUSE:
                if (g_state == STATE_PLAYING) {
                    ahi_stop_all();
                    g_state = STATE_PAUSED;
                    update_display();
                }
                break;

            case GAD_STOP:
                player_stop();
                break;

            case GAD_PREV:
                if (g_cur > 0) player_start(g_cur - 1);
                else if (g_state == STATE_PLAYING) player_seek(0);
                break;

            case GAD_NEXT:
                if (g_cur < g_pcount - 1) player_start(g_cur + 1);
                break;

            case GAD_REW:
                /* Rewind 30s — utilise bpf (bytes/frame) pour éviter overflow */
                if (g_state == STATE_PLAYING && g_mp3src && g_mp3_decoded > 0) {
                    ULONG frames = BE32(g_sc->frames_decoded);
                    ULONG bpf    = (frames > 0) ? (g_mp3_decoded / frames) : 417UL;
                    ULONG skip   = (30UL * g_sr) / 1152UL;
                    ULONG newf   = (frames > skip) ? (frames - skip) : 0;
                    ULONG pos    = newf * bpf;   /* <= g_mp3sz, pas d'overflow */
                    if (pos >= (ULONG)g_mp3sz) pos = 0;
                    stream_close();
                    stream_open(pos);
                }
                break;

            case GAD_FF:
                /* Fast-forward 30s */
                if (g_state == STATE_PLAYING && g_mp3src && g_mp3_decoded > 0) {
                    ULONG frames = BE32(g_sc->frames_decoded);
                    ULONG bpf    = (frames > 0) ? (g_mp3_decoded / frames) : 417UL;
                    ULONG skip   = (30UL * g_sr) / 1152UL;
                    ULONG pos    = (frames + skip) * bpf;
                    if (pos < (ULONG)g_mp3sz) {
                        stream_close();
                        stream_open(pos);
                    }
                }
                break;

            case GAD_OPEN: {
                /* ASL file selector */
                struct FileRequester *fr;
                struct TagItem fr_tags[] = {
                    {ASLFR_TitleText,     (ULONG)"Select MP3"},
                    {ASLFR_DoMultiSelect, TRUE},
                    {TAG_END,             0}
                };
                fr = AllocAslRequest(ASL_FileRequest, fr_tags);
                if (fr && AslRequest(fr, NULL)) {
                    int fi, added = 0;
                    /* Multi-select path: fr_NumArgs entries in fr_ArgList */
                    if (fr->fr_NumArgs > 0 && fr->fr_ArgList != NULL) {
                        for (fi = 0; fi < (int)fr->fr_NumArgs && g_pcount < MAX_PLAYLIST; fi++) {
                            char *full = (char *)AllocMem(PATH_BUF_SIZE, MEMF_ANY | MEMF_CLEAR);
                            if (full) {
                                int dl = (int)strlen(fr->fr_Drawer);
                                if (dl > PATH_BUF_SIZE - 2) dl = PATH_BUF_SIZE - 2;
                                memcpy(full, fr->fr_Drawer, (size_t)dl);
                                if (dl > 0 && full[dl-1] != '/' && full[dl-1] != ':')
                                    full[dl++] = '/';
                                strncpy(full + dl, fr->fr_ArgList[fi].wa_Name,
                                        (size_t)(PATH_BUF_SIZE - 1 - dl));
                                full[PATH_BUF_SIZE - 1] = '\0';
                                g_playlist[g_pcount++] = full;
                                added++;
                            }
                        }
                    }
                    /* Single-file fallback: use fr_File + fr_Drawer */
                    if (added == 0 && fr->fr_File && fr->fr_File[0] && g_pcount < MAX_PLAYLIST) {
                        char *full = (char *)AllocMem(PATH_BUF_SIZE, MEMF_ANY | MEMF_CLEAR);
                        if (full) {
                            int dl = (int)strlen(fr->fr_Drawer);
                            if (dl > PATH_BUF_SIZE - 2) dl = PATH_BUF_SIZE - 2;
                            memcpy(full, fr->fr_Drawer, (size_t)dl);
                            if (dl > 0 && full[dl-1] != '/' && full[dl-1] != ':')
                                full[dl++] = '/';
                            strncpy(full + dl, fr->fr_File,
                                    (size_t)(PATH_BUF_SIZE - 1 - dl));
                            full[PATH_BUF_SIZE - 1] = '\0';
                            g_playlist[g_pcount++] = full;
                            added++;
                        }
                    }
                    /* Auto-start first new track if stopped */
                    if (added > 0 && g_state == STATE_STOPPED) {
                        int first = g_pcount - added;
                        player_start(first);
                    }
                }
                if (fr) FreeAslRequest(fr);
                break;
            }

            case GAD_LOOP:
                g_loop = !g_loop;
                break;

            case GAD_PLS:
                /* Toggle fenêtre playlist */
                if (g_plswin) {
                    CloseWindow(g_plswin); g_plswin = NULL;
                    if (g_plsglist) { FreeGadgets(g_plsglist); g_plsglist = NULL; }
                } else {
                    open_playlist_window();
                }
                break;

            case GAD_PROGRESS:
                /* Seek vers la position cliquée */
                if (g_mp3src && g_mp3sz > 0)
                    player_seek((ULONG)code);
                break;
            }
            /* Redraw icons after any button release (button un-highlights itself
             * before GADGETUP is delivered, so DrawImage lands on clean face) */
            if (gad->GadgetID >= GAD_PREV && gad->GadgetID <= GAD_LOOP)
                draw_icons(g_win);
        }

        /* Raccourcis clavier */
        if (class == IDCMP_RAWKEY && !(code & 0x80)) {
            switch (code) {
            case 0x40: /* Espace : play/pause bascule */
                if (g_state == STATE_PLAYING) {
                    ahi_stop_all(); g_state = STATE_PAUSED; update_display();
                } else if (g_state == STATE_PAUSED) {
                    UBYTE *buf = (g_acur == 0) ? g_buf0 : g_buf1;
                    g_state = STATE_PLAYING;
                    send_ahi_chunk(g_acur, buf, g_chunk);
                } else if (g_state == STATE_STOPPED && g_pcount > 0) {
                    int t = (g_cur >= 0 && g_cur < g_pcount) ? g_cur : 0;
                    player_start(t);
                }
                break;
            case 0x45: /* Echap : stop */
                player_stop();
                break;
            case 0x4F: /* Flèche gauche : piste précédente */
                if (g_cur > 0) player_start(g_cur - 1);
                break;
            case 0x4E: /* Flèche droite : piste suivante */
                if (g_cur < g_pcount - 1) player_start(g_cur + 1);
                break;
            }
        }
    }
}

/* ================================================================
 * build_gui — construit la fenêtre et les gadgets
 * ================================================================ */
/* ================================================================
 * Playlist window — LISTVIEW_KIND GadTools
 * ================================================================ */
static void open_playlist_window(void)
{
    struct NewGadget ng;
    struct Gadget   *prev;
    struct TagItem   lv_tags[4];
    struct TagItem   win_tags[11];
    UWORD bx, by, ih;
    int   i;

    if (g_plswin) return;                   /* déjà ouverte */
    if (!g_scr || !g_vi) return;

    /* Initialise exec List (NewList inline — no prototype in this NDK) */
    g_pls_execlist.lh_Head     = (struct Node *)&g_pls_execlist.lh_Tail;
    g_pls_execlist.lh_Tail     = NULL;
    g_pls_execlist.lh_TailPred = (struct Node *)&g_pls_execlist;
    for (i = 0; i < g_pcount; i++) {
        g_pls_nodes[i].ln_Name = g_playlist[i];   /* chemin complet */
        g_pls_nodes[i].ln_Type = NT_USER;
        g_pls_nodes[i].ln_Pri  = 0;
        AddTail(&g_pls_execlist, &g_pls_nodes[i]);
    }

    bx = g_scr->WBorLeft;
    by = g_scr->WBorTop + g_scr->Font->ta_YSize + 1;
    ih = 100;   /* hauteur zone listview */

    prev = CreateContext(&g_plsglist);
    if (!prev) return;

    ng.ng_LeftEdge   = bx;
    ng.ng_TopEdge    = by;
    ng.ng_Width      = 300 - bx - g_scr->WBorRight;
    ng.ng_Height     = ih;
    ng.ng_GadgetText = NULL;
    ng.ng_TextAttr   = g_scr->Font;
    ng.ng_GadgetID   = 1;
    ng.ng_Flags      = 0;
    ng.ng_VisualInfo = g_vi;

    lv_tags[0].ti_Tag  = GTLV_Labels;       lv_tags[0].ti_Data = (ULONG)&g_pls_execlist;
    lv_tags[1].ti_Tag  = GTLV_ScrollWidth;  lv_tags[1].ti_Data = 16;
    lv_tags[2].ti_Tag  = GTLV_ShowSelected; lv_tags[2].ti_Data = 0;
    lv_tags[3].ti_Tag  = TAG_END;           lv_tags[3].ti_Data = 0;

    prev = g_plsgad = CreateGadgetA(LISTVIEW_KIND, prev, &ng, lv_tags);
    if (!prev) { FreeGadgets(g_plsglist); g_plsglist = NULL; return; }

    win_tags[0].ti_Tag  = WA_Left;      win_tags[0].ti_Data = 40;
    win_tags[1].ti_Tag  = WA_Top;       win_tags[1].ti_Data = 160;
    win_tags[2].ti_Tag  = WA_Width;     win_tags[2].ti_Data = 300;
    win_tags[3].ti_Tag  = WA_Height;    win_tags[3].ti_Data = (ULONG)(by + ih + g_scr->WBorBottom + 2);
    win_tags[4].ti_Tag  = WA_Title;     win_tags[4].ti_Data = (ULONG)"Playlist";
    win_tags[5].ti_Tag  = WA_Gadgets;   win_tags[5].ti_Data = (ULONG)g_plsglist;
    win_tags[6].ti_Tag  = WA_Flags;     win_tags[6].ti_Data =
                           WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET |
                           WFLG_ACTIVATE | WFLG_SMART_REFRESH;
    win_tags[7].ti_Tag  = WA_IDCMP;     win_tags[7].ti_Data =
                           IDCMP_CLOSEWINDOW | IDCMP_GADGETUP;
    win_tags[8].ti_Tag  = WA_PubScreen; win_tags[8].ti_Data = (ULONG)g_scr;
    win_tags[9].ti_Tag  = TAG_END;      win_tags[9].ti_Data = 0;

    g_plswin = OpenWindowTagList(NULL, win_tags);
    if (!g_plswin) {
        FreeGadgets(g_plsglist); g_plsglist = NULL;
        return;
    }
    GT_RefreshWindow(g_plswin, NULL);
}

/* Traitement IDCMP de la fenêtre playlist — appelé dans la boucle principale */
static void handle_pls_idcmp(void)
{
    struct IntuiMessage *msg;
    if (!g_plswin) return;

    while ((msg = (struct IntuiMessage *)GetMsg(g_plswin->UserPort))) {
        ULONG class = msg->Class;
        UWORD code  = msg->Code;
        ReplyMsg((struct Message *)msg);

        if (class == IDCMP_CLOSEWINDOW) {
            CloseWindow(g_plswin); g_plswin = NULL;
            if (g_plsglist) { FreeGadgets(g_plsglist); g_plsglist = NULL; }
            return;
        }
        if (class == IDCMP_GADGETUP && (int)code < g_pcount) {
            player_start((int)code);
            /* Mettre en surbrillance la piste courante */
            if (g_plsgad && g_plswin)
                GT_SetGadgetAttrs(g_plsgad, g_plswin, NULL,
                                  GTLV_Selected, (ULONG)code, TAG_END);
        }
    }
}

/* ================================================================
 * build_gui — construit la fenêtre et les gadgets
 * ================================================================ */
static BOOL build_gui(void)
{
    struct NewGadget ng;
    struct Gadget   *prev;
    UWORD bx, by, iw;
    UWORD fh;    /* hauteur de police */
    UWORD btn_w, btn_h;
    int   i, win_h;
    struct TagItem win_tags[16];

    memset(g_gad, 0, sizeof(g_gad));

    g_scr = LockPubScreen(NULL);
    if (!g_scr) return FALSE;

    g_vi = GetVisualInfoA(g_scr, NULL);
    if (!g_vi) { UnlockPubScreen(NULL, g_scr); g_scr = NULL; return FALSE; }

    fh  = g_scr->Font->ta_YSize;
    bx  = g_scr->WBorLeft;
    by  = g_scr->WBorTop + fh + 1;
    iw  = WIN_WIDTH - bx - g_scr->WBorRight;

    /* Hauteur d'une ligne de texte */
    g_th = fh + 2;

    /* Positions des lignes texte (coord. fenêtre absolues) */
    g_tx   = bx + 2;
    g_ty[0] = by + 2;
    g_ty[1] = g_ty[0] + g_th;
    g_ty[2] = g_ty[1] + g_th;

    /* Progress slider y = après les 3 lignes texte + petit gap */
    UWORD prog_y = g_ty[2] + g_th + 2;
    UWORD btn_y  = prog_y + 14;

    btn_w = (iw - 9 * 2) / 10; /* 10 boutons avec 2px de gap */
    btn_h = 12;

    /* btn_y est déjà absolu (inclut by) — hauteur fenêtre = bas des boutons + bord bas */
    win_h = (int)btn_y + (int)btn_h + (int)g_scr->WBorBottom + 4;

    /* --- Création de la liste de gadgets --- */
    prev = CreateContext(&g_glist);
    if (!prev) goto fail;

    /* Barre de progression (SLIDER horizontal) */
    ng.ng_LeftEdge   = bx + 2;
    ng.ng_TopEdge    = prog_y;
    ng.ng_Width      = iw - 4;
    ng.ng_Height     = 12;
    ng.ng_GadgetText = NULL;
    ng.ng_TextAttr   = g_scr->Font;
    ng.ng_GadgetID   = GAD_PROGRESS;
    ng.ng_Flags      = 0;
    ng.ng_VisualInfo = g_vi;
    {
        struct TagItem sl_tags[] = {
            {GTSL_Min,         0},
            {GTSL_Max,         PROGRESS_MAX},
            {GTSL_Level,       0},
            {GTSL_MaxLevelLen, 5},
            {GA_RelVerify,     TRUE},
            {TAG_END,          0}
        };
        prev = g_gad[GAD_PROGRESS] = CreateGadgetA(SLIDER_KIND, prev, &ng, sl_tags);
    }
    if (!prev) goto fail;

    /* 10 transport buttons — no text label (icons drawn separately) */
    for (i = 1; i <= 10; i++) {
        ng.ng_LeftEdge   = bx + 2 + (UWORD)((i - 1) * (btn_w + 2));
        ng.ng_TopEdge    = btn_y;
        ng.ng_Width      = btn_w;
        ng.ng_Height     = btn_h;
        ng.ng_GadgetText = NULL;   /* no text; DrawImage used instead */
        ng.ng_TextAttr   = g_scr->Font;
        ng.ng_GadgetID   = (UWORD)i;
        ng.ng_Flags      = 0;      /* no PLACETEXT_IN */
        ng.ng_VisualInfo = g_vi;
        prev = g_gad[i] = CreateGadgetA(BUTTON_KIND, prev, &ng, NULL);
        if (!prev) goto fail;
    }

    /* Ouvrir la fenêtre */
    win_tags[0].ti_Tag  = WA_Left;       win_tags[0].ti_Data = 40;
    win_tags[1].ti_Tag  = WA_Top;        win_tags[1].ti_Data = 40;
    win_tags[2].ti_Tag  = WA_Width;      win_tags[2].ti_Data = WIN_WIDTH;
    win_tags[3].ti_Tag  = WA_Height;     win_tags[3].ti_Data = (ULONG)win_h;
    win_tags[4].ti_Tag  = WA_Title;      win_tags[4].ti_Data = (ULONG)"ZZPlayGUI XX16";
    win_tags[5].ti_Tag  = WA_Gadgets;    win_tags[5].ti_Data = (ULONG)g_glist;
    win_tags[6].ti_Tag  = WA_Flags;      win_tags[6].ti_Data =
                            WFLG_DRAGBAR | WFLG_CLOSEGADGET | WFLG_DEPTHGADGET |
                            WFLG_ACTIVATE | WFLG_SMART_REFRESH;
    win_tags[7].ti_Tag  = WA_IDCMP;      win_tags[7].ti_Data =
                            IDCMP_CLOSEWINDOW | IDCMP_GADGETUP | IDCMP_RAWKEY;
    win_tags[8].ti_Tag  = WA_PubScreen;  win_tags[8].ti_Data = (ULONG)g_scr;
    win_tags[9].ti_Tag  = TAG_END;       win_tags[9].ti_Data = 0;

    g_win = OpenWindowTagList(NULL, win_tags);
    if (!g_win) goto fail;

    /* Refresh gadgets then overlay transport icons */
    GT_RefreshWindow(g_win, NULL);
    draw_icons(g_win);

    /* Sauvegarder la largeur interne pour update_display */
    g_iw = (WORD)(g_win->Width - g_win->BorderLeft - g_win->BorderRight);

    UnlockPubScreen(NULL, g_scr);
    /* g_scr reste valide tant que la fenêtre est ouverte */
    return TRUE;

fail:
    if (g_vi)    { FreeVisualInfo(g_vi); g_vi = NULL; }
    if (g_glist) { FreeGadgets(g_glist); g_glist = NULL; }
    UnlockPubScreen(NULL, g_scr);
    g_scr = NULL;
    return FALSE;
}

/* ================================================================
 * init_ahi — ouvre ahi.device, prépare 2 AHIRequest
 * ================================================================ */
static BOOL init_ahi(void)
{
    int i;
    g_ahiport = CreateMsgPort();
    if (!g_ahiport) return FALSE;

    for (i = 0; i < 2; i++) {
        g_req[i] = (struct AHIRequest *)
                   CreateIORequest(g_ahiport, sizeof(struct AHIRequest));
        if (!g_req[i]) return FALSE;
        g_act[i] = FALSE;
    }
    g_req[0]->ahir_Version = 4;
    if (OpenDevice(AHINAME, AHI_DEFAULT_UNIT,
                   (struct IORequest *)g_req[0], 0) != 0)
        return FALSE;
    CopyMem(g_req[0], g_req[1], sizeof(struct AHIRequest));
    return TRUE;
}

static void cleanup_ahi(void)
{
    int i;
    ahi_stop_all();
    for (i = 0; i < 2; i++) {
        if (g_req[i]) {
            if (i == 0) CloseDevice((struct IORequest *)g_req[i]);
            DeleteIORequest((struct IORequest *)g_req[i]);
            g_req[i] = NULL;
        }
    }
    if (g_ahiport) { DeleteMsgPort(g_ahiport); g_ahiport = NULL; }
}

/* ================================================================
 * main
 * ================================================================ */
int main(int argc, char *argv[])
{
    int rc = 0, i;

    /* Priority 20 — matches input.device, prevents preemption by WB menus */
    SetTaskPri(FindTask(NULL), 20);

    /* Librairies — IntuitionBase/GfxBase ont leurs propres struct dans le NDK */
    IntuitionBase = (struct IntuitionBase *)OpenLibrary("intuition.library", 37);
    GfxBase       = (struct GfxBase *)      OpenLibrary("graphics.library",  37);
    GadToolsBase  =                         OpenLibrary("gadtools.library",  37);
    AslBase       =                         OpenLibrary("asl.library",       37);

    if (!IntuitionBase || !GfxBase || !GadToolsBase || !AslBase) {
        PutStr("zzplay-gui: missing libraries (OS 3.1 required)\n");
        rc = 20; goto bye_libs;
    }

    /* ZZ9000 */
    {
        struct ConfigDev *cd = FindConfigDev(NULL, ZZ9000_MANUF, ZZ9000_PROD_AX);
        if (!cd) cd = FindConfigDev(NULL, ZZ9000_MANUF, ZZ9000_PROD);
        if (!cd) {
            PutStr("zzplay-gui: ZZ9000 not found\n");
            rc = 10; goto bye_libs;
        }
        g_board = (UBYTE *)cd->cd_BoardAddr;
        g_fb    = g_board + MNT_FB_BASE;
    }

    /* AHI */
    if (!init_ahi()) {
        PutStr("zzplay-gui: ahi.device not found\n");
        rc = 10; goto bye_libs;
    }

    /* Initialise icon bitmaps (static data, no allocation) */
    init_btn_images();

    /* GUI */
    if (!build_gui()) {
        PutStr("zzplay-gui: window open failed\n");
        rc = 10; goto bye_ahi;
    }

    /* Playlist initiale depuis argv — allocation taille fixe PATH_BUF_SIZE */
    for (i = 1; i < argc && g_pcount < MAX_PLAYLIST; i++) {
        char *s = (char *)AllocMem(PATH_BUF_SIZE, MEMF_ANY | MEMF_CLEAR);
        if (s) {
            strncpy(s, argv[i], PATH_BUF_SIZE - 1);
            g_playlist[g_pcount++] = s;
        }
    }

    strcpy(g_dispname, "Ready - OP to load");
    draw_row(0, g_dispname);
    draw_row(1, "ZZPlayGUI XX16 / Xanxi 2026");
    draw_row(2, "");

    /* Démarrage automatique si playlist fournie */
    if (g_pcount > 0) {
        g_cur = 0;
        player_start(0);
    }

    /* ============================================================
     * BOUCLE PRINCIPALE
     * ============================================================ */
    while (!g_quit) {
        if (g_state == STATE_PLAYING) {
            BOOL cont = player_tick();
            if (!cont && !g_quit) {
                /* Fin de piste — avancement automatique */
                if (g_cur < g_pcount - 1) {
                    player_start(g_cur + 1);
                } else if (g_loop && g_pcount > 0) {
                    /* Loop mode: restart from beginning of playlist */
                    player_start(0);
                } else {
                    strcpy(g_dispname, "End of playlist");
                    draw_row(0, g_dispname);
                    draw_row(1, "");
                    draw_row(2, "");
                }
            }
        } else {
            /* Arrêté ou en pause : attendre événement fenêtre principale + playlist */
            ULONG sigs = 1L << g_win->UserPort->mp_SigBit;
            if (g_plswin)
                sigs |= 1L << g_plswin->UserPort->mp_SigBit;
            Wait(sigs);
        }
        handle_idcmp();
        handle_pls_idcmp();
    }

    /* ============================================================
     * Nettoyage
     * ============================================================ */
    player_stop();

    for (i = 0; i < g_pcount; i++)
        if (g_playlist[i]) { FreeMem(g_playlist[i], PATH_BUF_SIZE); g_playlist[i] = NULL; }

    /* Fermer la fenêtre playlist si ouverte */
    if (g_plswin) { CloseWindow(g_plswin); g_plswin = NULL; }
    if (g_plsglist) { FreeGadgets(g_plsglist); g_plsglist = NULL; }

    if (g_win) {
        CloseWindow(g_win);
        g_win = NULL;
    }
    if (g_glist) { FreeGadgets(g_glist);   g_glist = NULL; }
    if (g_vi)    { FreeVisualInfo(g_vi);   g_vi    = NULL; }

bye_ahi:
    cleanup_ahi();

bye_libs:
    if (AslBase)       { CloseLibrary(AslBase);       AslBase       = NULL; }
    if (GadToolsBase)  { CloseLibrary(GadToolsBase);  GadToolsBase  = NULL; }
    if (GfxBase)       { CloseLibrary((struct Library *)GfxBase);       GfxBase       = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }

    return rc;
}
