/*
 * ZZMP3Play.c
 * MULTIPLIER=8, prefill 75%, CacheClearU()
 * Pour firmware avec backpressure modifié (pcm_quantum_bytes * 8)
 *
 * Audio pipeline: improved double-buffer scheduling (ported from ZZPlayGUI).
 *   OLD (fragile): WaitIO(cur) → fill next → SendIO(next)
 *   NEW (robust) : fill next → SendIO(next) → WaitIO(cur)
 * SendIO happens BEFORE WaitIO so AHI already has the next buffer queued
 * at interrupt level when the current one ends — no task scheduling gap.
 * ahir_Link chains next behind cur inside AHI for truly gapless transitions.
 *
 * Build (two-step required — one-step fails with this toolchain):
 *   m68k-amigaos-gcc -O2 -noixemul -m68020 -c -o ZZMP3Play.o ZZMP3Play.c
 *   m68k-amigaos-gcc      -noixemul -m68020 -o ZZMP3Play ZZMP3Play.o -lamiga
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <devices/ahi.h>
#include <libraries/configvars.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/ahi.h>

#include <string.h>
#include <stdio.h>

#define MULTIPLIER 8
#define MP3_LOW_WATER      (64UL * 1024UL)
#define MP3_PUSH_SIZE      (256UL * 1024UL)

#define ZZ9000_MANUF         0x6D6E
#define ZZ9000_PROD_AX       0x0A
#define ZZ9000_PROD          0x04

#define MNT_FB_BASE          0x00010000UL
#define REG_CMD              0x64

#define XACP_STREAM_OFFSET   0x04002000UL
#define XACP_MP3_OFFSET      0x04100000UL
#define XACP_PCM_OFFSET      0x04200000UL

#define MP3_RING_SIZE        (512UL * 1024UL)
#define PCM_RING_SIZE        (1024UL * 1024UL)

#define OP_STREAM_OPEN       4
#define OP_STREAM_CLOSE      5
#define STREAM_DECODE_DONE   2

#define ZZ_WR(b,o,v)  (*((volatile UWORD*)((UBYTE*)(b)+(o))) = (UWORD)(v))
#define ZZ_RD(b,o)    (*((volatile UWORD*)((UBYTE*)(b)+(o))))

typedef struct {
    volatile ULONG  mp3_base;
    volatile ULONG  mp3_size;
    volatile ULONG  mp3_write;
    volatile ULONG  mp3_read;
    volatile ULONG  mp3_need_refill;
    volatile ULONG  mp3_eof;
    volatile ULONG  pcm_base;
    volatile ULONG  pcm_size;
    volatile ULONG  pcm_write;
    volatile ULONG  pcm_read;
    volatile ULONG  sample_rate;
    volatile ULONG  channels;
    volatile ULONG  status;
    volatile ULONG  error;
    volatile ULONG  underrun_count;
    volatile ULONG  frames_decoded;
    volatile ULONG  flags;
} XACP_StreamControl;

#define BE32(x) (x)

static UBYTE buffer0[65536];
static UBYTE buffer1[65536];
static struct AHIRequest *ahireq[2];
static struct MsgPort    *ahiport;
static BOOL               ahi_active[2];

static ULONG ring_avail(ULONG write, ULONG read, ULONG size) {
    if (write >= read) return write - read;
    return size - read + write;
}

static ULONG ring_free(ULONG write, ULONG read, ULONG size) {
    return (read + size - write - 1) % size;
}

static void ring_push(UBYTE *ring, ULONG ring_size,
                      ULONG *wp, const UBYTE *src, ULONG len)
{
    ULONG w = *wp;
    ULONG ste = ring_size - w;
    if (len <= ste) {
        CopyMem((APTR)src, ring + w, len);
    } else {
        CopyMem((APTR)src, ring + w, ste);
        CopyMem((APTR)(src + ste), ring, len - ste);
    }
    *wp = (w + len) % ring_size;
}

static ULONG get_pcm_write(XACP_StreamControl *sc) {
    CacheClearU();
    return BE32(sc->pcm_write);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        Printf("Usage: %s <fichier.mp3> [debug]\n", (ULONG)argv[0]);
        return 1;
    }
    SetTaskPri(FindTask(NULL), 20);   /* same priority as input.device — prevents WB menu preemption */
    BOOL debug = (argc >= 3);

    struct ConfigDev *cd = FindConfigDev(NULL, ZZ9000_MANUF, ZZ9000_PROD_AX);
    if (!cd) cd = FindConfigDev(NULL, ZZ9000_MANUF, ZZ9000_PROD);
    if (!cd) { Printf("ZZ9000 introuvable\n"); return 1; }

    UBYTE *board = (UBYTE *)cd->cd_BoardAddr;
    UBYTE *fb    = board + MNT_FB_BASE;
    Printf("board=0x%08lx  fb=0x%08lx\n", (ULONG)board, (ULONG)fb);

    BPTR fh = Open((STRPTR)argv[1], MODE_OLDFILE);
    if (!fh) { Printf("Impossible d'ouvrir %s\n", (ULONG)argv[1]); return 1; }

    Seek(fh, 0, OFFSET_END);
    LONG mp3_file_size = Seek(fh, 0, OFFSET_BEGINNING);
    if (mp3_file_size <= 0) { Printf("Fichier vide\n"); Close(fh); return 1; }

    UBYTE *mp3_src = (UBYTE *)AllocMem(mp3_file_size, MEMF_ANY);
    if (!mp3_src) { Printf("AllocMem %ld echec\n", mp3_file_size); Close(fh); return 1; }
    Read(fh, mp3_src, mp3_file_size);
    Close(fh);
    Printf("MP3 charge : %ld octets\n", mp3_file_size);

    UBYTE             *mp3_ring = fb + XACP_MP3_OFFSET;
    UBYTE             *pcm_ring = fb + XACP_PCM_OFFSET;
    XACP_StreamControl *sc     = (XACP_StreamControl *)(fb + XACP_STREAM_OFFSET);

    memset((void *)sc, 0, sizeof(XACP_StreamControl));

    sc->mp3_base = BE32(XACP_MP3_OFFSET);
    sc->mp3_size = BE32(MP3_RING_SIZE);
    sc->pcm_base = BE32(XACP_PCM_OFFSET);
    sc->pcm_size = BE32(PCM_RING_SIZE);

    ULONG src_pos   = 0;
    ULONG mp3_wptr  = 0;
    ULONG prefill   = (MP3_RING_SIZE * 3UL) / 4UL;
    if ((ULONG)mp3_file_size < prefill) prefill = (ULONG)mp3_file_size;

    ring_push(mp3_ring, MP3_RING_SIZE, &mp3_wptr, mp3_src, prefill);
    src_pos = prefill;
    sc->mp3_write = BE32(mp3_wptr);
    Printf("Prefill MP3 : %lu KB\n", prefill / 1024UL);

    ZZ_WR(board, REG_CMD, OP_STREAM_OPEN);
    Printf("OP_STREAM_OPEN envoyé\n");

    { volatile int d = 20000; while(d--); }

    {
        ULONG timeout = 0;
        while (sc->sample_rate == 0) {
            volatile int d = 1000; while(d--);
            if (++timeout > 200000) {
                Printf("TIMEOUT sample_rate\n");
                goto cleanup;
            }
        }
    }

    ULONG sr  = BE32(sc->sample_rate);
    ULONG ch  = BE32(sc->channels);
    if (sr == 0) sr = 44100;
    if (ch == 0 || ch > 2) ch = 2;
    Printf("sample_rate=%lu Hz  channels=%lu\n", sr, ch);

    ULONG quantum_frames = sr / 50;
    ULONG quantum_bytes = quantum_frames * ch * 2;
    ULONG chunk_bytes = quantum_bytes * MULTIPLIER;
    Printf("Quantum ARM : %lu frames (%lu octets)\n", quantum_frames, quantum_bytes);
    Printf("Chunk AHI   : %lu octets (MULTIPLIER=%d)\n", chunk_bytes, MULTIPLIER);

    ahiport = CreateMsgPort();
    if (!ahiport) { Printf("CreateMsgPort echec\n"); goto cleanup; }

    {
        int i;
        for (i = 0; i < 2; i++) {
            ahireq[i] = (struct AHIRequest *)CreateIORequest(ahiport, sizeof(struct AHIRequest));
            ahi_active[i] = FALSE;
        }
        if (OpenDevice("ahi.device", AHI_DEFAULT_UNIT, (struct IORequest *)ahireq[0], 0) != 0) {
            Printf("ahi.device introuvable\n");
            goto cleanup_ahi;
        }
        for (i = 1; i < 2; i++)
            CopyMem(ahireq[0], ahireq[i], sizeof(struct AHIRequest));
    }

    ULONG ahi_type = (ch == 2) ? AHIST_S16S : AHIST_M16S;
    Printf("AHI type : %s, chunk=%lu octets\n",
           (ULONG)((ch == 2) ? "STEREO" : "MONO"), chunk_bytes);

    ULONG pcm_rd    = 0;
    BOOL  eof_sent  = FALSE;
    BOOL  done      = FALSE;
    BOOL  quit      = FALSE;
    ULONG loops     = 0;

    /* Premier chunk : attendre que le ring ait assez de PCM */
    {
        ULONG pcm_w = get_pcm_write(sc);
        ULONG avail = ring_avail(pcm_w, pcm_rd, PCM_RING_SIZE);
        while (avail < chunk_bytes) {
            Delay(1);
            pcm_w = get_pcm_write(sc);
            avail = ring_avail(pcm_w, pcm_rd, PCM_RING_SIZE);
            if (CheckSignal(SIGBREAKF_CTRL_C)) { quit = TRUE; break; }
        }
        if (quit) goto cleanup_ahi;

        ULONG space_to_end = PCM_RING_SIZE - pcm_rd;
        if (chunk_bytes <= space_to_end) {
            CopyMem((APTR)(pcm_ring + pcm_rd), buffer0, chunk_bytes);
        } else {
            ULONG part2 = chunk_bytes - space_to_end;
            CopyMem((APTR)(pcm_ring + pcm_rd), buffer0, space_to_end);
            CopyMem((APTR)pcm_ring, buffer0 + space_to_end, part2);
        }
        pcm_rd = (pcm_rd + chunk_bytes) % PCM_RING_SIZE;
        sc->pcm_read = BE32(pcm_rd);

        ahireq[0]->ahir_Std.io_Command = CMD_WRITE;
        ahireq[0]->ahir_Std.io_Data    = buffer0;
        ahireq[0]->ahir_Std.io_Length  = chunk_bytes;
        ahireq[0]->ahir_Frequency      = sr;
        ahireq[0]->ahir_Type           = ahi_type;
        ahireq[0]->ahir_Volume         = 0x10000L;
        ahireq[0]->ahir_Position       = 0x8000L;
        ahireq[0]->ahir_Link           = NULL;
        SendIO((struct IORequest *)ahireq[0]);
        ahi_active[0] = TRUE;
    }

    int cur = 0, next = 1;
    while (!quit) {
        loops++;
        if (CheckSignal(SIGBREAKF_CTRL_C)) { quit = TRUE; break; }

        if (!done && BE32(sc->status) == STREAM_DECODE_DONE)
            done = TRUE;

        if (!eof_sent && BE32(sc->mp3_need_refill)) {
            ULONG rptr   = BE32(sc->mp3_read);
            ULONG mfree  = ring_free(mp3_wptr, rptr, MP3_RING_SIZE);
            ULONG mavail = (ULONG)mp3_file_size - src_pos;
            ULONG push   = MP3_PUSH_SIZE;
            if (push > mavail) push = mavail;
            if (push > mfree)  push = mfree;
            if (push > 0) {
                ring_push(mp3_ring, MP3_RING_SIZE, &mp3_wptr,
                          mp3_src + src_pos, push);
                src_pos += push;
                sc->mp3_write = BE32(mp3_wptr);
                sc->mp3_need_refill = 0;
            }
        }

        if (!eof_sent && src_pos >= (ULONG)mp3_file_size) {
            sc->mp3_eof = BE32(1);
            eof_sent = TRUE;
        }

        ULONG pcm_w = get_pcm_write(sc);
        ULONG avail = ring_avail(pcm_w, pcm_rd, PCM_RING_SIZE);
        if (done && avail == 0) {
            quit = TRUE;
            break;
        }

        ULONG chunk = chunk_bytes;
        if (done && avail < chunk_bytes) {
            chunk = avail & ~3UL;
            if (chunk == 0) {
                quit = TRUE;
                break;
            }
        }

        while (avail < chunk) {
            Delay(1);
            pcm_w = get_pcm_write(sc);
            avail = ring_avail(pcm_w, pcm_rd, PCM_RING_SIZE);
            if (CheckSignal(SIGBREAKF_CTRL_C)) { quit = TRUE; break; }
            if (done && avail == 0) break;
        }
        if (quit) break;

        UBYTE *buf = (next == 0) ? buffer0 : buffer1;
        ULONG space_to_end = PCM_RING_SIZE - pcm_rd;
        if (chunk <= space_to_end) {
            CopyMem((APTR)(pcm_ring + pcm_rd), buf, chunk);
        } else {
            ULONG part2 = chunk - space_to_end;
            CopyMem((APTR)(pcm_ring + pcm_rd), buf, space_to_end);
            CopyMem((APTR)pcm_ring, buf + space_to_end, part2);
        }
        pcm_rd = (pcm_rd + chunk) % PCM_RING_SIZE;
        sc->pcm_read = BE32(pcm_rd);

        /* Send next — chained behind cur for gapless double-buffering.
         * AHI queues next at interrupt level so there is no scheduling gap
         * between the two buffers regardless of task preemption. */
        ahireq[next]->ahir_Std.io_Command = CMD_WRITE;
        ahireq[next]->ahir_Std.io_Data    = buf;
        ahireq[next]->ahir_Std.io_Length  = chunk;
        ahireq[next]->ahir_Frequency      = sr;
        ahireq[next]->ahir_Type           = ahi_type;
        ahireq[next]->ahir_Volume         = 0x10000L;
        ahireq[next]->ahir_Position       = 0x8000L;
        ahireq[next]->ahir_Link           = ahi_active[cur] ? ahireq[cur] : NULL;
        SendIO((struct IORequest *)ahireq[next]);
        ahi_active[next] = TRUE;

        /* Wait for cur — next is already in AHI's queue, no gap possible */
        if (ahi_active[cur]) {
            WaitIO((struct IORequest *)ahireq[cur]);
            ahi_active[cur] = FALSE;
        }

        int tmp = cur; cur = next; next = tmp;

        if (debug && (loops & 31) == 0) {
            Printf("[DBG] chunk=%lu avail=%lu pcm_rd=%lu frames=%lu\n",
                   chunk, avail, pcm_rd, BE32(sc->frames_decoded));
        }
    }

    if (ahi_active[cur]) WaitIO((struct IORequest *)ahireq[cur]);
    if (ahi_active[next]) WaitIO((struct IORequest *)ahireq[next]);

    Printf("=== Fin ===\n");
    Printf("Frames ARM    : %lu\n", BE32(sc->frames_decoded));
    Printf("Underruns ARM : %lu\n", BE32(sc->underrun_count));
    Printf("Boucles 68k   : %lu\n", loops);

cleanup_ahi:
    sc->mp3_eof = BE32(1);
    ZZ_WR(board, REG_CMD, OP_STREAM_CLOSE);
    Delay(1);

    for (int i = 0; i < 2; i++) {
        if (ahireq[i]) {
            if (ahi_active[i]) {
                if (!CheckIO((struct IORequest *)ahireq[i]))
                    AbortIO((struct IORequest *)ahireq[i]);
                WaitIO((struct IORequest *)ahireq[i]);
            }
            if (i == 0) CloseDevice((struct IORequest *)ahireq[i]);
            DeleteIORequest((struct IORequest *)ahireq[i]);
        }
    }
    if (ahiport) DeleteMsgPort(ahiport);
cleanup:
    if (mp3_src) FreeMem(mp3_src, mp3_file_size);
    return 0;
}