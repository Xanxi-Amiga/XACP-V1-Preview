/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>

#include "platform.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xil_io.h"
#include "xscugic.h"
#include "xgpiops.h"
#include "sleep.h"
#include "xil_cache.h"
#include "xil_exception.h"
#include "xclk_wiz.h"
#include "xtime_l.h"
#include "xi2stx.h"
#include "xi2srx.h"

// workaround for typo in xilinx C code
void Xil_AssertNonVoid() {}

#include "memorymap.h"
#include "mntzorro.h"
#include "video.h"
#include "hdmi.h"
#include "gfx.h"
#include "ethernet.h"
#include "usb.h"
#include "interrupt.h"
#include "bootrom.h"
#include "core2.h"
#include "adc.h"
#include "ax.h"
#include "mp3/mp3.h"
#include "mp3/minimp3.h"

#include "zz_regs.h"
#include "zz_video_modes.h"

#define REVISION_MAJOR 1
#define REVISION_MINOR 13

#define GPIO_DEVICE_ID	XPAR_XGPIOPS_0_DEVICE_ID

void disable_reset_out() {
	XGpioPs Gpio;
	XGpioPs_Config *ConfigPtr;
	ConfigPtr = XGpioPs_LookupConfig(GPIO_DEVICE_ID);
	XGpioPs_CfgInitialize(&Gpio, ConfigPtr, ConfigPtr->BaseAddr);
	int output_pin = 7;

	XGpioPs_SetDirectionPin(&Gpio, output_pin, 1);
	XGpioPs_SetOutputEnablePin(&Gpio, output_pin, 1);
	XGpioPs_WritePin(&Gpio, output_pin, 0);
	usleep(10000);
	XGpioPs_WritePin(&Gpio, output_pin, 1);
	print("[gpio] ethernet reset done.\r\n");

	// FIXME
	int adau_reset = 11;
	XGpioPs_SetDirectionPin(&Gpio, adau_reset, 1);
	XGpioPs_SetOutputEnablePin(&Gpio, adau_reset, 1);
	XGpioPs_WritePin(&Gpio, adau_reset, 0);
	usleep(10000);
	XGpioPs_WritePin(&Gpio, adau_reset, 1);

	print("[gpio] ADAU reset done.\r\n");
}

u32 blitter_colormode = MNTVA_COLOR_32BIT;
static u32 blitter_dst_offset = 0;
static u32 blitter_src_offset = 0;

struct ZZ_VIDEO_STATE* video_state;

/* === XACP internal state variables === */
static mp3dec_t xanxi_mp3d;
static int xanxi_initialized = 0;
static uint32_t x_in_off  = 0;
static uint32_t x_out_off = 0;
static uint32_t x_in_size = 0;
static uint8_t  x_pending_param = 0; /* state machine for 0x64+0x68 register pair */
static uint16_t x_status = 0;
static uint16_t x_result = 0;
static uint16_t x_rate = 0;
static uint16_t x_channels = 0;
static uint16_t x_error = 0;
static uint16_t x_debug_hi = 0;
static uint16_t x_debug_lo = 0;
static int xacp_mp3_job_pending = 0;  /* deferred MP3 decode job */
static int xacp_stream_active   = 0;  /* streaming mode active    */
/* === end XACP internal state variables === */

/* === XACP — Xanxi ARM Coprocessor Protocol — V1 === */
#define XACP_CMD_OFFSET    0x04000000UL
#define XACP_STREAM_OFFSET 0x04002000UL  /* XACP_StreamControl */

/* Expected ring buffer offsets — validated at STREAM_OPEN */
#define XACP_MP3_BASE_EXPECTED  0x04100000UL
#define XACP_MP3_SIZE_EXPECTED  0x00080000UL  /* 512 KB */
#define XACP_PCM_BASE_EXPECTED  0x04200000UL
#define XACP_PCM_SIZE_EXPECTED  0x00100000UL  /* 1 MB  */

/* ── 0x0000–0x00FF : Core / System ─────────────────────────────── */
#define OP_MEMCPY              1   /* ARM-accelerated DDR copy       */
#define OP_MANDELBROT          2   /* Mandelbrot/Julia render        */
/* 0x03–0x0F : reserved Core/System */

/* ── 0x0100–0x01FF : Stream / Audio / DSP ──────────────────────── */
#define OP_MP3_DECODE          3   /* Deferred full-file MP3 decode  */
#define OP_STREAM_OPEN         4   /* MP3 ring streaming — open      */
#define OP_STREAM_CLOSE        5   /* MP3 ring streaming — close     */
/* OP_STREAM_RESET : deprecated legacy alias.
 * Correct pattern is CLOSE → OPEN. Do not add state machine here.
 * Kept only for binary compatibility with old clients. */
#define OP_STREAM_RESET        6   /* DEPRECATED — use CLOSE+OPEN    */
/* Reserved Audio/DSP — not yet implemented */
#define OP_SID_RENDER          0x0110  /* RESERVED — ZZ SID synth    */
#define OP_MIDI_SF2            0x0120  /* RESERVED — MIDI SF2 synth  */
#define OP_DSP_EQ              0x0130  /* RESERVED — software EQ     */
#define OP_PCM_RESAMPLE        0x0140  /* RESERVED — PCM resample    */

/* ── 0x0200–0x02FF : Image / RTG / Datatypes ───────────────────── */
#define OP_IMG_SCALE           0x0200  /* RESERVED — image scaling   */
#define OP_CHUNKY_TO_PLANAR    0x0210  /* RESERVED — c2p             */
#define OP_PLANAR_TO_CHUNKY    0x0211  /* RESERVED — p2c             */
#define OP_DATATYPE_DECODE     0x0220  /* RESERVED — datatype decode */

/* ── 0x0300–0x03FF : Core1 / Execution ─────────────────────────── */
#define OP_CORE1_START         0x0300  /* RESERVED — Core1 blob run  */
#define OP_CORE1_STOP          0x0301  /* RESERVED — Core1 halt      */
#define OP_CORE1_STATUS        0x0302  /* RESERVED — Core1 poll      */
#define OP_PICODRIVE_LOAD      0x0310  /* RESERVED — PicoDrive ROM   */
#define OP_DOSBOX_START        0x0320  /* RESERVED — DOSBox instance */
#define OP_MPEG_DECODE         0x0330  /* RESERVED — MPEG video      */
#define OP_ARM_QUAKE           0x0340  /* RESERVED — ARM Quake       */

/* ── 0x0400–0x04FF : Compression / Archive ─────────────────────── */
#define OP_ZIP_DECOMPRESS      0x0401  /* RESERVED — ZIP/DEFLATE     */
#define OP_INFLATE             0x0400  /* RESERVED — raw DEFLATE     */
#define OP_LHA_EXTRACT         0x0410  /* RESERVED — LHA extract     */
#define OP_LZX_DECOMPRESS      0x0420  /* RESERVED — LZX decompress  */

/* ── 0x0500–0x05FF : Crypto / Hash ─────────────────────────────── */
#define OP_HASH_MD5            0x0500  /* RESERVED — MD5             */
#define OP_HASH_SHA256         0x0501  /* RESERVED — SHA-256         */
#define OP_AES_DECRYPT         0x0510  /* RESERVED — AES decrypt     */

/* Status codes */
#define XACP_STATUS_IDLE         0
#define XACP_STATUS_BUSY         1
#define XACP_STATUS_DONE         2
#define XACP_STATUS_ERROR        3
#define XACP_ERR_NOT_IMPLEMENTED 0xFFFF

/* Stream states */
#define STREAM_IDLE         0
#define STREAM_STREAMING    1
#define STREAM_DECODE_DONE  2
#define STREAM_ERROR        3

/* Generic command structure — used by all single-shot opcodes */
typedef struct {
    uint32_t opcode;
    uint32_t input_offset;
    uint32_t input_size;
    uint32_t output_offset;
    uint32_t output_max_size;
    uint32_t param1;
    uint32_t param2;
    uint32_t param3;
    uint32_t param4;
    uint32_t result1;
    uint32_t result2;
    uint32_t status;
    uint32_t error;
} XACP_Command;

/* Structure streaming — ring MP3 + ring PCM (XX14)
 *
 * MP3 input : ring buffer.
 *   mp3_write : owned by 68k — write head (advances as 68k pushes data)
 *   mp3_read  : owned by ARM — read head (advances as ARM consumes)
 *   ARM available = (mp3_write - mp3_read) mod mp3_size
 *
 * Advantage vs XX10 sliding window :
 *   - no memmove on 68k side
 *   - no need_refill/ack protocol
 *   - simple LOW_WATER : mp3_avail < threshold → mp3_need_refill=1
 */
typedef struct {
    /* MP3 input — ring buffer */
    uint32_t mp3_base;         /* DDR offset of MP3 ring (fixed, written by 68k before STREAM_OPEN) */
    uint32_t mp3_size;         /* MP3 ring total size (fixed) */
    uint32_t mp3_write;        /* [68k-owned] write head */
    uint32_t mp3_read;         /* [ARM-owned] read head  */
    uint32_t mp3_need_refill;  /* ARM→68k : 1 = low watermark, needs more data */
    uint32_t mp3_eof;          /* [68k-owned] 1 = end of file, no more data */

    /* PCM output — ring buffer */
    uint32_t pcm_base;         /* DDR offset of PCM ring */
    uint32_t pcm_size;         /* PCM ring total size */
    uint32_t pcm_write;        /* [ARM-owned] write head */
    uint32_t pcm_read;         /* [68k-owned] read head */

    /* Audio info (filled by ARM after first decoded frame) */
    uint32_t sample_rate;
    uint32_t channels;

    /* State */
    uint32_t status;           /* IDLE/STREAMING/DECODE_DONE/ERROR */
    uint32_t error;
    uint32_t underrun_count;
    uint32_t frames_decoded;
    uint32_t flags;            /* bit0 = underrun_occurred */
} XACP_StreamControl;

/* Endianness : 68k ecrit big-endian, ARM lit little-endian */
static inline uint32_t xacp_be32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) <<  8) |
           ((v & 0x00FF0000u) >>  8) |
           ((v & 0xFF000000u) >> 24);
}
#define be32(v)        xacp_be32(v)
#define cpu_to_be32(v) xacp_be32(v)

/* === end XACP === */

// cur_mem_offset : legacy framebuffer transfer register offset
// still referenced by dma_acc.c
unsigned int cur_mem_offset = 0x3500000;
int interrupt_enabled_vblank = 0;

static char usb_storage_available = 0;
static uint32_t usb_storage_read_block = 0;
static uint32_t usb_storage_write_block = 0;

// ethernet state
uint16_t ethernet_send_result = 0;
int eth_backlog_nag_counter = 0;
int interrupt_enabled_ethernet = 0;

// usb state
uint16_t usb_status = 0;
uint32_t usb_read_write_num_blocks = 1;
// debug things like individual reads/writes, greatly slowing the system down
uint32_t debug_lowlevel = 0;

// audio state (ZZ9000AX)
static int audio_buffer_collision = 0;
static uint32_t audio_scale = 48000/50;
static uint32_t audio_offset = 0;
static int adau_enabled = 0;
int interrupt_enabled_audio = 0;

// debug test state
static uint32_t zz_debug_test_counter = 0;
static uint32_t zz_debug_test_prev = 0;
static uint32_t zz_debug_test_ms = 0;

void handle_amiga_reset() {
	printf("    _______________   ___   ___   ___  \n");
	printf("   |___  /___  / _ \\ / _ \\ / _ \\ / _ \\ \n");
	printf("      / /   / / (_) | | | | | | | | | |\n");
	printf("     / /   / / \\__, | | | | | | | | | |\n");
	printf("    / /__ / /__  / /| |_| | |_| | |_| |\n");
	printf("   /_____/_____|/_/  \\___/ \\___/ \\___/ \n\n");

	video_reset();

	// stop audio
	audio_set_tx_buffer((uint8_t*)AUDIO_TX_BUFFER_ADDRESS);
	audio_silence();
	audio_set_rx_buffer((uint8_t*)AUDIO_RX_BUFFER_ADDRESS);

	// usb
	usb_storage_available = zz_usb_init();
	usb_status = 0;
	usb_read_write_num_blocks = 1;

	// ethernet
	ethernet_send_result = 0;
	eth_backlog_nag_counter = 0;
	interrupt_enabled_ethernet = 0;
	interrupt_enabled_audio = 0;

	cur_mem_offset = 0x3500000;

	// FIXME
	memset((u32 *)Z3_SCRATCH_ADDR, 0, sizeof(struct GFXData));

	// clear audio buffer on reset
	memset((void*)AUDIO_TX_BUFFER_ADDRESS, 0, AUDIO_TX_BUFFER_SIZE);

	// FIXME test content for audio buffer
	/*int16_t* adata = (uint16_t*)(((void*)AUDIO_TX_BUFFER_ADDRESS));
	float f = 1;
	for (int i=0; i<AUDIO_TX_BUFFER_SIZE/2; i++) {
		adata[i] = (sin((float)i/200.0)*65536)*f;
		f-=0.0001;
	}*/

	// reset ADAU
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 8 | 0);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 8 | 4);
	mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG5, 0);

	adau_enabled = audio_adau_init(1);

	// clear interrupt holding amiga
	amiga_interrupt_clear(0xffffffff);

	// Used for testing the nonstandard VSync modes without the driver having to enable them.
	//card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC] = 1;
}

int main() {
	init_platform();

	boot_rom_init();

	disable_reset_out();

	video_state = video_init();

	xadc_init();

	interrupt_configure();

	ethernet_init();

	fpga_interrupt_connect(isr_video, isr_audio, isr_audio_rx);

	handle_amiga_reset();

	// ARM app run environment
	arm_app_init();
	volatile struct ZZ9K_ENV* arm_run_env = arm_app_get_run_env();
	uint32_t arm_run_address = 0;

	// graphics temporary registers
	uint16_t rect_x1 = 0;
	uint16_t rect_x2 = 0;
	uint16_t rect_x3 = 0;
	uint16_t rect_y1 = 0;
	uint16_t rect_y2 = 0;
	uint16_t rect_y3 = 0;
	uint16_t blitter_dst_pitch = 0;
	uint32_t rect_rgb = 0;
	uint32_t rect_rgb2 = 0;
	uint32_t blitter_colormode = MNTVA_COLOR_32BIT;
	uint32_t blitter_colormode_hibyte = 0;
	uint16_t blitter_src_pitch = 0;
	uint16_t blitter_user1 = 0;
	uint16_t blitter_user2 = 0;

	// custom video mode
	int custom_video_mode = ZZVMODE_CUSTOM;
	int custom_vmode_param = VMODE_PARAM_HRES;

	// zorro state
	u32 zstate_raw;
	int need_req_ack = 0;

	// audio parameters (buffer locations)
	uint16_t audio_params[ZZ_NUM_AUDIO_PARAMS];
	int audio_param = 0; // selected parameter
	int audio_request_init = 0;

	// decoder parameters (mp3 etc)
	const int ZZ_NUM_DECODER_PARAMS = 8;
	uint16_t decoder_params[ZZ_NUM_DECODER_PARAMS];
	int decoder_param = 0; // selected parameter
	int decoder_bytes_decoded = 0;

	// idle task counter (used for ethernet negotiation)
	int idle_task_count = 0;

	while (1) {
		u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
		if (debug_lowlevel && (zstate_raw&0xff)!=(zstate&0xff)) {
			printf("ZSTATE: %x\n", zstate);
		}
		zstate_raw = zstate;
		u32 writereq = (zstate & (1 << 31));
		u32 readreq = (zstate & (1 << 30));

		if (writereq) {
			u32 zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);
			u32 zdata = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG1);

			u32 ds3 = (zstate_raw & (1 << 29));
			u32 ds2 = (zstate_raw & (1 << 28));
			u32 ds1 = (zstate_raw & (1 << 27));
			u32 ds0 = (zstate_raw & (1 << 26));

			if (debug_lowlevel) {
				printf("WRTE: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);
			}

			if (zaddr > 0x10000000) {
				printf("ERRW illegal address %08lx\n", zaddr);
			} else if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = (u8*)FRAMEBUFFER_ADDRESS;

				if (zaddr >= MNT_FB_BASE) {
					ptr = ptr + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// NOP (RX frame is here)
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					// 0x8000 - 0x9fff ETH TX frame (Z2)
					ptr = (u8*)TX_FRAME_ADDRESS + zaddr - MNT_REG_BASE - 0x8000;
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000 - 0xffff USB block storage (Z2)
					ptr = (u8*)USB_BLOCK_STORAGE_ADDRESS + zaddr - MNT_REG_BASE - 0xa000;
				}

				// FIXME cache this
				u32 z3 = (zstate_raw & (1 << 25));

				if (z3) {
					if (ds3) ptr[0] = zdata >> 24;
					if (ds2) ptr[1] = zdata >> 16;
					if (ds1) ptr[2] = zdata >> 8;
					if (ds0) ptr[3] = zdata;
				} else {
					// swap bytes
					if (ds1) ptr[0] = zdata >> 8;
					if (ds0) ptr[1] = zdata;
				}
			} else if (zaddr >= MNT_REG_BASE && zaddr < MNT_FB_BASE) {
				// register area
				//printf("REGW: %08lx <- %08lx [%d%d%d%d]\n",zaddr,zdata,!!ds3,!!ds2,!!ds1,!!ds0);

				u32 z3 = (zstate_raw & (1 << 25));
				if (z3) {
					// convert 32bit to 16bit addresses
					if (ds3 && ds2) {
						zdata = zdata >> 16;
					} else if (ds1 && ds0) {
						zdata = zdata & 0xffff;
						zaddr += 2;
					} else {
						zaddr = 0; // cancel
					}
				}
				//printf("CONV: %08lx <- %08lx\n",zaddr,zdata);

				switch (zaddr) {
				// Various blitter/video registers
				case REG_ZZ_PAN_HI:
					video_state->framebuffer_pan_offset = zdata << 16;
					break;
				case REG_ZZ_PAN_LO:
					video_state->framebuffer_pan_offset |= zdata;

					// cursor offset support for p96 panning
					video_state->sprite_x_offset = rect_x1;
					video_state->sprite_y_offset = rect_y1;

					// FIXME: document/comment this. rect_x1/x2/y1 are used for panning inside of a screen
					// together with blitter_colormode
					// TODO: rework to dedicated registers because this makes it hard to debug

					video_state->framebuffer_pan_width = rect_x2;
					u32 framebuffer_color_format = blitter_colormode;
					video_state->framebuffer_pan_offset += (rect_x1 << blitter_colormode);
					video_state->framebuffer_pan_offset += (rect_y1 * (video_state->framebuffer_pan_width << framebuffer_color_format));
					break;

				case REG_ZZ_BLIT_SRC_HI:
					blitter_src_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_SRC_LO:
					blitter_src_offset |= zdata;
					break;
				case REG_ZZ_BLIT_DST_HI:
					blitter_dst_offset = zdata << 16;
					break;
				case REG_ZZ_BLIT_DST_LO:
					blitter_dst_offset |= zdata;
					break;

				case REG_ZZ_COLORMODE:
					blitter_colormode = zdata & 0x0f;
					blitter_colormode_hibyte = zdata >> 8;
					break;
				case REG_ZZ_CONFIG:
					// enable/disable INT6, currently used to signal incoming ethernet packets
					if (zdata & 8) {
						// clear/ack
						if (zdata & 16) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_ETH);
						}
						if (zdata & 32) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_AUDIO);
						}
					} else {
						//printf("[enable] eth: %d\n", (int)zdata);
						interrupt_enabled_ethernet = zdata & 1;

						if (!interrupt_enabled_ethernet) {
							amiga_interrupt_clear(AMIGA_INTERRUPT_ETH);
						}
					}
					break;
				case REG_ZZ_MODE: {
					int mode = zdata & 0xff;
					int colormode = (zdata & 0xf00) >> 8;
					int scalemode = (zdata & 0xf000) >> 12;

					video_mode_init(mode, scalemode, colormode);

					// FIXME
					// remember selected video mode
					// video_mode = zdata;
					break;
				}
				case REG_ZZ_VCAP_MODE:
					printf("videocap default mode select: %lx\n", zdata);
					video_state->videocap_video_mode = zdata & 0xff;
					break;
				//case REG_ZZ_SPRITE_X:
				case REG_ZZ_SPRITE_Y:
					if (!video_state->sprite_showing)
						break;

					video_state->sprite_x_base = (int16_t)rect_x1;
					video_state->sprite_y_base = (int16_t)rect_y1;
					update_hw_sprite_pos();

					break;
				case REG_ZZ_SPRITE_BITMAP: {
					if (zdata == 1) { // Hardware sprite enabled
						hw_sprite_show(1);
						break;
					}
					else if (zdata == 2) { // Hardware sprite disabled
						hw_sprite_show(0);
						break;
					}

					uint8_t* bmp_data = (uint8_t*) ((u32) video_state->framebuffer
							+ blitter_src_offset);

					video_state->sprite_x_offset = rect_x1;
					video_state->sprite_y_offset = rect_y1;
					video_state->sprite_width  = rect_x2;
					video_state->sprite_height = rect_y2;

					clear_hw_sprite();
					update_hw_sprite(bmp_data, 0, 0);
					update_hw_sprite_pos();
					break;
				}
				case REG_ZZ_SPRITE_COLORS: {
					video_state->sprite_colors[zdata] = (blitter_user1 << 16) | blitter_user2;
					if (zdata != 0 && video_state->sprite_colors[zdata] == 0xff00ff)
						video_state->sprite_colors[zdata] = 0xfe00fe;
					break;
				}
				case REG_ZZ_SRC_PITCH:
					blitter_src_pitch = zdata;
					break;

				case REG_ZZ_X1:
					rect_x1 = zdata;
					break;
				case REG_ZZ_Y1:
					rect_y1 = zdata;
					break;
				case REG_ZZ_X2:
					rect_x2 = zdata;
					break;
				case REG_ZZ_Y2:
					rect_y2 = zdata;
					break;
				case REG_ZZ_ROW_PITCH:
					blitter_dst_pitch = zdata;
					break;
				case REG_ZZ_X3:
					rect_x3 = zdata;
					break;
				case REG_ZZ_Y3:
					rect_y3 = zdata;
					break;

				case REG_ZZ_USER1:
					blitter_user1 = zdata;
					break;
				case REG_ZZ_USER2:
					blitter_user2 = zdata;
					break;
				case REG_ZZ_USER3:
					// FIXME unused
					break;
				case REG_ZZ_USER4:
					// FIXME unused
					break;

/* === XACP write registers ===
 * XACP command state machine on 0x64 + 0x68 .
 * Sequence :
 *   ecrire 0x10 sur 0x64 → prochain write 0x68 = x_in_off  HI
 *   ecrire 0x11 sur 0x64 → prochain write 0x68 = x_in_off  LO
 *   ecrire 0x12 sur 0x64 → prochain write 0x68 = x_out_off HI
 *   ecrire 0x13 sur 0x64 → prochain write 0x68 = x_out_off LO
 *   ecrire 0x14 sur 0x64 → prochain write 0x68 = x_in_size HI
 *   ecrire 0x15 sur 0x64 → prochain write 0x68 = x_in_size LO
 *   ecrire 0x01 sur 0x64 → executer CMD_MEMCPY
 * ============================================================ */

case 0x68: { /* write selected parameter value */
    switch (x_pending_param) {
        case 0x10: x_in_off  = (x_in_off  & 0x0000FFFFu) | ((uint32_t)zdata << 16); break;
        case 0x11: x_in_off  = (x_in_off  & 0xFFFF0000u) |  (uint32_t)zdata;        break;
        case 0x12: x_out_off = (x_out_off & 0x0000FFFFu) | ((uint32_t)zdata << 16); break;
        case 0x13: x_out_off = (x_out_off & 0xFFFF0000u) |  (uint32_t)zdata;        break;
        case 0x14: x_in_size = (x_in_size & 0x0000FFFFu) | ((uint32_t)zdata << 16); break;
        case 0x15: x_in_size = (x_in_size & 0xFFFF0000u) |  (uint32_t)zdata;        break;
    }
    x_pending_param = 0;
    break;
}

/* === XACP V1 register protocol — Xanxi 2026 === */
#define CACHE_LINE_SIZE 32
#define ALIGN_DOWN(x, a) ((x) & ~((a)-1))
#define ALIGN_UP(x, a)   (((x) + (a)-1) & ~((a)-1))
#define X_FB_SIZE (128u * 1024u * 1024u)

case 0x64: {
    uint32_t cmd = zdata;

    /* Codes 0x10-0x15 : select next parameter target (written via 0x68) */
    if (cmd >= 0x10u && cmd <= 0x15u) {
        x_pending_param = (uint8_t)cmd;
        break;
    }

    x_status = 1; /* BUSY */

    if (cmd == 0) { /* CMD_RESET */
        mp3dec_init(&xanxi_mp3d);
        xanxi_initialized = 1;
        x_in_off = 0; x_out_off = 0; x_in_size = 0;
        x_status = 0; x_result = 0; x_error = 0;
        break;
    }

    if (cmd == OP_MANDELBROT) { /* XACP Mandelbrot via XACP_Command */
        uint8_t      *xacp_raw = (uint8_t*)video_state->framebuffer + XACP_CMD_OFFSET;
        XACP_Command *xcmd;
        uint32_t w, h, maxiter, pitch_b, fbptr;
        int32_t  xmin, xmax, ymin, ymax;
        uint8_t  *fb_base;
        uint32_t  py, px, chk = 0;

        /* Invalidate D-cache over XACP command struct (128 bytes = 4 cache lines) */
        {
            uintptr_t xs = ((uintptr_t)xacp_raw) & ~0x1FUL;
            uintptr_t xe = xs + 128UL;
            Xil_DCacheInvalidateRange(xs, (uint32_t)(xe - xs));
        }

        xcmd    = (XACP_Command*)xacp_raw;
        /* Mandelbrot mapping — champs semantiquement corrects :
         * output_offset   = offset framebuffer destination
         * output_max_size = pitch en bytes
         * param1          = width
         * param2          = height
         * param3          = max_iter
         * param4          = xmin_fp (Q16.16)
         * input_offset    = xmax_fp (reutilise — TODO: union)
         * input_size      = ymin_fp (reutilise — TODO: union)
         * input_offset+4  = ymax_fp -> utilise param4 supplementaire
         * Pour l'instant on lit comme avant, a nettoyer avec union  */
        fbptr   = be32(xcmd->output_offset);
        pitch_b = be32(xcmd->output_max_size);
        w       = be32(xcmd->param1);
        h       = be32(xcmd->param2);
        maxiter = be32(xcmd->param3);
        xmin    = (int32_t)be32(xcmd->param4);
        xmax    = (int32_t)be32(xcmd->input_offset);
        ymin    = (int32_t)be32(xcmd->input_size);
        ymax    = (int32_t)be32(xcmd->result1);

        /* Validation */
        if (w == 0 || w > 1920 || h == 0 || h > 1200 ||
            maxiter == 0 || maxiter > 4096 || pitch_b == 0 ||
            fbptr >= X_FB_SIZE) {
            x_error  = 0xE6;
            x_status = 3; /* STATUS_ERROR */
            break;
        }

        fb_base = (uint8_t*)video_state->framebuffer + fbptr;

        /* Q16.16 fixed-point to float for VFP computation */
        {
            float fxmin = (float)xmin / 65536.0f;
            float fxmax = (float)xmax / 65536.0f;
            float fymin = (float)ymin / 65536.0f;
            float fymax = (float)ymax / 65536.0f;
            float fdx   = (fxmax - fxmin) / (float)w;
            float fdy   = (fymax - fymin) / (float)h;

            for (py = 0; py < h; py++) {
                float fci = fymin + (float)py * fdy;
                uint32_t *row = (uint32_t*)(fb_base + py * pitch_b);
                for (px = 0; px < w; px++) {
                    float fcr = fxmin + (float)px * fdx;
                    float fzr = 0.0f, fzi = 0.0f;
                    uint32_t it = 0;
                    while (it < maxiter) {
                        float fzr2 = fzr * fzr;
                        float fzi2 = fzi * fzi;
                        if (fzr2 + fzi2 > 4.0f) break;
                        fzi = 2.0f * fzr * fzi + fci;
                        fzr = fzr2 - fzi2 + fcr;
                        it++;
                    }
                    {
                        uint32_t pix;
                        if (it >= maxiter) {
                            pix = 0xFF000000u;
                        } else {
                            uint8_t t = (uint8_t)((it & 31u) << 3);
                            uint8_t r, g, b;
                            switch ((it >> 5) & 3u) {
                                case 0: r=0;   g=0;     b=t;     break;
                                case 1: r=0;   g=t;     b=255;   break;
                                case 2: r=t;   g=255;   b=255-t; break;
                                default:r=255; g=255-t; b=0;     break;
                            }
                            pix = 0xFF000000u | ((uint32_t)r<<16)
                                              | ((uint32_t)g<<8) | b;
                        }
                        row[px] = pix;
                        chk += pix;
                    }
                }
            }
        }

        /* No framebuffer flush needed : ZZ9000 DDR is uncached or
         * auto-flushed — image is visible without explicit flush.
         * A 2MB flush caused 112s stall. */

        x_result   = (uint16_t)(chk & 0xFFFFu);
        x_rate     = (uint16_t)(chk >> 16);
        x_channels = (uint16_t)(w & 0xFFFFu);
        x_error    = 0;
        x_status   = 2; /* STATUS_DONE */
        break;
    }

    if (cmd == OP_STREAM_OPEN) { /* XACP streaming init */
        uint8_t            *sc_raw = (uint8_t*)video_state->framebuffer + XACP_STREAM_OFFSET;
        XACP_StreamControl *sc;

        /* Invalidate D-cache over StreamControl */
        {
            uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
            Xil_DCacheInvalidateRange(xs, 128);
        }

        sc = (XACP_StreamControl*)sc_raw;

        /* XX16 — strict offset validation.
         * Any mismatch = STREAM_ERROR : prevents RTG framebuffer corruption. */
        if (be32(sc->mp3_base) != XACP_MP3_BASE_EXPECTED ||
            be32(sc->mp3_size) != XACP_MP3_SIZE_EXPECTED  ||
            be32(sc->pcm_base) != XACP_PCM_BASE_EXPECTED  ||
            be32(sc->pcm_size) != XACP_PCM_SIZE_EXPECTED) {
            sc->status = cpu_to_be32(STREAM_ERROR);
            sc->error  = cpu_to_be32(0xBAD0BAD0u);
            {
                uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
                Xil_DCacheFlushRange(xs, 128);
            }
            x_error  = 0xE9;
            x_status = 3;
            break;
        }

        /* Init minimp3 et activer streaming */
        mp3dec_init(&xanxi_mp3d);
        xacp_stream_active = 1;

        /* XX14 : init ARM-owned MP3 read pointer. */
        sc->mp3_read        = cpu_to_be32(0u);
        sc->mp3_need_refill = cpu_to_be32(0u);

        /* XX16 — reset pcm_write (ARM-owned).
         * A stale value would trigger immediate HIGH_WATER or bogus wrap. */
        sc->pcm_write       = cpu_to_be32(0u);

        /* Marquer STREAMING */
        sc->status          = cpu_to_be32(STREAM_STREAMING);
        sc->underrun_count  = cpu_to_be32(0u);
        sc->frames_decoded  = cpu_to_be32(0u);
        sc->flags           = cpu_to_be32(0u);
        sc->sample_rate     = cpu_to_be32(0u);
        sc->channels        = cpu_to_be32(0u);

        /* Flush StreamControl */
        {
            uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
            Xil_DCacheFlushRange(xs, 128);
        }

        x_status = 2; /* STATUS_DONE — immediate ACK */
        break;
    }

    if (cmd == OP_STREAM_CLOSE) { /* XACP streaming stop */
        xacp_stream_active = 0;
        mp3dec_init(&xanxi_mp3d); /* reset minimp3 state */
        x_status = 2;
        break;
    }

    if (cmd == OP_STREAM_RESET) {
        /* DEPRECATED. Correct pattern: CLOSE → OPEN.
         * Replay/reopen logic belongs in ZZmpega.library, not here.
         * For binary compatibility only: behaves as CLOSE. */
        xacp_stream_active = 0;
        mp3dec_init(&xanxi_mp3d);
        x_status = 2;
        break;
    }

    /* Reserved opcodes — not yet implemented.
     * All opcodes >= 0x0100 that are not handled above return NOT_IMPLEMENTED. */
    if (cmd >= 0x0100) {
        x_status = XACP_STATUS_ERROR;
        x_error  = XACP_ERR_NOT_IMPLEMENTED;
        break;
    }

    if (cmd == OP_MP3_DECODE) { /* XACP MP3 decode — deferred, hors handler write */
        uint8_t      *xacp_raw = (uint8_t*)video_state->framebuffer + XACP_CMD_OFFSET;
        XACP_Command *xcmd;
        uint32_t in_off, in_size, out_off, out_max;

        /* Invalidate D-cache over XACP command struct */
        {
            uintptr_t xs = ((uintptr_t)xacp_raw) & ~0x1FUL;
            Xil_DCacheInvalidateRange(xs, 128);
        }

        xcmd     = (XACP_Command*)xacp_raw;
        in_off   = be32(xcmd->input_offset);
        in_size  = be32(xcmd->input_size);
        out_off  = be32(xcmd->output_offset);
        out_max  = be32(xcmd->output_max_size);

        /* Validation rapide */
        if (in_size == 0 || in_off  >= X_FB_SIZE ||
            in_off  + in_size  > X_FB_SIZE ||
            out_off >= X_FB_SIZE ||
            out_off + out_max  > X_FB_SIZE) {
            x_error  = 0xE8;
            x_status = 3; /* STATUS_ERROR */
            break;
        }

        /* Marquer BUSY et programmer le job — ACK immediatement.
         * Le decode se fera dans la boucle idle, pas dans ce handler.
         * Cela evite de bloquer le bus Zorro pendant toute la duree du decode. */
        x_status = 1; /* BUSY — visible via ZZ_RD(0x64) */
        xcmd->status = cpu_to_be32(1u); /* BUSY in struct as well */
        {
            uintptr_t xs = ((uintptr_t)xacp_raw) & ~0x1FUL;
            Xil_DCacheFlushRange(xs, 64);
        }
        xacp_mp3_job_pending = 1;
        break;
    }

    if (x_in_size == 0) {
        x_status = 0xE2;
        x_error  = 0xAA;
        break;
    }

    if (x_in_off >= X_FB_SIZE || x_in_size > (X_FB_SIZE - x_in_off)) {
        x_status = 0xE4;
        x_error  = 0xCC;
        break;
    }

    if (x_out_off >= X_FB_SIZE || x_in_size > (X_FB_SIZE - x_out_off)) {
        x_status = 0xE5;
        x_error  = 0xCD;
        break;
    }

    uint8_t *src_ptr = (uint8_t*)video_state->framebuffer + x_in_off;
    uint8_t *dst_ptr = (uint8_t*)video_state->framebuffer + x_out_off;

    /* Invalidate source cache with strict 32-byte alignment */
    uintptr_t s_start = ((uintptr_t)src_ptr) & ~0x1FUL;
    uintptr_t s_end   = ((uintptr_t)src_ptr + x_in_size + 31UL) & ~0x1FUL;
    Xil_DCacheInvalidateRange(s_start, (uint32_t)(s_end - s_start));

    /* Capture first 4 bytes as seen by ARM after cache invalidation */
    x_debug_hi = ((uint16_t)src_ptr[0] << 8) | src_ptr[1];
    x_debug_lo = ((uint16_t)src_ptr[2] << 8) | src_ptr[3];

    /* Checksum of source buffer */
    {
        uint32_t cs = 0, i;
        for (i = 0; i < x_in_size; i++)
            cs += src_ptr[i];
        x_rate     = (uint16_t)(cs & 0xFFFFu);
        x_channels = (uint16_t)(cs >> 16);
    }

    if (cmd == 1) { /* CMD_MEMCPY */
        memcpy(dst_ptr, src_ptr, x_in_size);

        /* Flush destination so the Amiga sees the result */
        uintptr_t d_start = ((uintptr_t)dst_ptr) & ~0x1FUL;
        uintptr_t d_end   = ((uintptr_t)dst_ptr + x_in_size + 31UL) & ~0x1FUL;
        Xil_DCacheFlushRange(d_start, (uint32_t)(d_end - d_start));

        x_result = (uint16_t)x_in_size;
        x_error  = 0;
        x_status = 2; /* STATUS_DONE */

    } else {
        x_status = 3; x_error = 0xFF; /* unknown command */
    }
    break;
}
/* === end XACP write registers === */

				case REG_ZZ_RGB_HI:
					rect_rgb &= 0xffff0000;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB_LO:
					rect_rgb &= 0x0000ffff;
					rect_rgb |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;
				case REG_ZZ_RGB2_HI:
					rect_rgb2 &= 0xffff0000;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8);
					break;
				case REG_ZZ_RGB2_LO:
					rect_rgb2 &= 0x0000ffff;
					rect_rgb2 |= (((zdata & 0xff) << 8) | zdata >> 8) << 16;
					break;

				// Generic acceleration ops
				case REG_ZZ_ACC_OP: {
					handle_acc_op(zdata);
					break;
				}

				// DMA RTG rendering
				case REG_ZZ_BLITTER_DMA_OP: {
					handle_blitter_dma_op(video_state, zdata);
					break;
				}

				// RTG rendering
				case REG_ZZ_FILLRECT:
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					uint8_t mask = zdata;

					if (mask == 0xFF)
						fill_rect_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								rect_rgb, blitter_colormode);
					else
						fill_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_rgb,
								blitter_colormode, mask);
					break;

				case REG_ZZ_COPYRECT: {
					mask = blitter_colormode_hibyte;
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					switch (zdata) {
					case 1: // Regular BlitRect
						if (mask == 0xFF || (mask != 0xFF && (blitter_colormode != MNTVA_COLOR_8BIT)))
							copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
											rect_y3, blitter_colormode,
											(uint32_t*) ((u32)video_state->framebuffer
													+ blitter_dst_offset),
											blitter_dst_pitch, MINTERM_SRC);
						else
							copy_rect(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
									rect_y3, blitter_colormode,
									(uint32_t*) ((u32)video_state->framebuffer
											+ blitter_dst_offset),
									blitter_dst_pitch, mask);
						break;
					case 2: // BlitRectNoMaskComplete
						copy_rect_nomask(rect_x1, rect_y1, rect_x2, rect_y2, rect_x3,
										rect_y3, blitter_colormode,
										(uint32_t*) ((u32)video_state->framebuffer
												+ blitter_src_offset),
										blitter_src_pitch, mask); // Mask in this case is minterm/opcode.
						break;
					}

					break;
				}

				case REG_ZZ_FILLTEMPLATE: {
					uint8_t draw_mode = blitter_colormode_hibyte;
					uint8_t* tmpl_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					uint8_t bpp = 2 * blitter_colormode;
					if (bpp == 0)
						bpp = 1;
					uint16_t loop_rows = 0;
					mask = zdata;

					if (zdata & 0x8000) {
						// pattern mode
						// TODO yoffset
						loop_rows = zdata & 0xff;
						mask = blitter_user1;
						blitter_src_pitch = 16;
						pattern_fill_rect(blitter_colormode, rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch, loop_rows);
					}
					else {
						template_fill_rect(blitter_colormode, rect_x1,
								rect_y1, rect_x2, rect_y2, draw_mode, mask,
								rect_rgb, rect_rgb2, rect_x3, rect_y3, tmpl_data,
								blitter_src_pitch);
					}

					break;
				}

				case REG_ZZ_SCRATCH_COPY: { // Copy from scratch area
					// FIXME for what?
					for (int i = 0; i < rect_y1; i++) {
						memcpy	((uint32_t*) ((u32)video_state->framebuffer + video_state->framebuffer_pan_offset + (i * rect_x1)),
								 (uint32_t*) ((u32)Z3_SCRATCH_ADDR + (i * rect_x1)),
								 rect_x1);
					}
					break;
				}

				case REG_ZZ_CVMODE_PARAM: // Custom video mode param
					// FIXME
					custom_vmode_param = zdata;
					break;

				case REG_ZZ_CVMODE_VAL: { // Custom video mode data
					struct zz_video_mode* vm = get_custom_video_mode_ptr(custom_video_mode);
					int *target = &vm->hres;
					switch(custom_vmode_param) {
						case VMODE_PARAM_VRES: target = &vm->vres; break;
						case VMODE_PARAM_HSTART: target = &vm->hstart; break;
						case VMODE_PARAM_HEND: target = &vm->hend; break;
						case VMODE_PARAM_HMAX: target = &vm->hmax; break;
						case VMODE_PARAM_VSTART: target = &vm->vstart; break;
						case VMODE_PARAM_VEND: target = &vm->vend; break;
						case VMODE_PARAM_VMAX: target = &vm->vmax; break;
						case VMODE_PARAM_POLARITY: target = &vm->polarity; break;
						case VMODE_PARAM_MHZ: target = &vm->mhz; break;
						case VMODE_PARAM_PHZ: target = &vm->phz; break;
						case VMODE_PARAM_VHZ: target = &vm->vhz; break;
						case VMODE_PARAM_HDMI: target = &vm->hdmi; break;
						case VMODE_PARAM_MUL: target = &vm->mul; break;
						case VMODE_PARAM_DIV: target = &vm->div; break;
						case VMODE_PARAM_DIV2: target = &vm->div2; break;
						default: break;
					}

					*target = zdata;
					break;
				}

				case REG_ZZ_CVMODE_SEL: // Set custom video mode index
					custom_video_mode = zdata;
					break;

				case REG_ZZ_CVMODE: // Set custom video mode without any questions asked.
					// This assumes that the custom video mode is 640x480 or higher resolution.
					video_mode_init(custom_video_mode, video_state->scalemode, video_state->colormode);
					break;

				case REG_ZZ_SET_FEATURE:
					switch (blitter_user1) {
						case CARD_FEATURE_SECONDARY_PALETTE:
							printf("[feature] SECONDARY_PALETTE: %lu\n",zdata);
							// Enables/disables the secondary palette on screen split with P96 3.10+
							video_state->card_feature_enabled[CARD_FEATURE_SECONDARY_PALETTE] = zdata;
							break;
						case CARD_FEATURE_NONSTANDARD_VSYNC:
							printf("[feature] NONSTANDARD_VSYNC: %lu\n",zdata);
							// Enables/disables the nonstandard refresh rates for scandoubled PAL/NTSC HDMI output modes.
							if (zdata == 2) {
								video_state->scandoubler_mode_adjust = 2;
							} else {
								video_state->scandoubler_mode_adjust = 0;
							}
							video_state->card_feature_enabled[CARD_FEATURE_NONSTANDARD_VSYNC] = zdata;
							break;
						default:
							break;
					}
					break;

				case REG_ZZ_P2C: {
					uint8_t draw_mode = blitter_colormode_hibyte;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					p2c_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask,
							layer_mask, blitter_src_pitch, bmp_data);
					break;
				}

				case REG_ZZ_P2D: {
					uint8_t draw_mode = blitter_colormode_hibyte;
					uint8_t planes = (zdata & 0xFF00) >> 8;
					uint8_t mask = (zdata & 0xFF);
					uint8_t layer_mask = blitter_user2;
					uint8_t* bmp_data = (uint8_t*) ((u32)video_state->framebuffer
							+ blitter_src_offset);

					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					p2d_rect(rect_x1, 0, rect_x2, rect_y2, rect_x3,
							rect_y3, draw_mode, planes, mask, layer_mask, rect_rgb,
							blitter_src_pitch, bmp_data, blitter_colormode);
					break;
				}

				case REG_ZZ_DRAWLINE: {
					uint8_t draw_mode = blitter_colormode_hibyte;
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);

					// rect_x3 contains the pattern. if all bits are set for both the mask and the pattern,
					// there's no point in passing non-essential data to the pattern/mask aware function.

					if (rect_x3 == 0xFFFF && zdata == 0xFF)
						draw_line_solid(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_rgb,
								blitter_colormode);
					else
						draw_line(rect_x1, rect_y1, rect_x2, rect_y2,
								blitter_user1, rect_x3, rect_y3, rect_rgb,
								rect_rgb2, blitter_colormode, zdata,
								draw_mode);
					break;
				}

				case REG_ZZ_INVERTRECT:
					set_fb((uint32_t*) ((u32)video_state->framebuffer + blitter_dst_offset),
							blitter_dst_pitch);
					invert_rect(rect_x1, rect_y1, rect_x2, rect_y2,
							zdata & 0xFF, blitter_colormode);
					break;

				case REG_ZZ_SET_SPLIT_POS:
					video_state->bgbuf_offset = blitter_src_offset;
					video_state->split_request_pos = zdata;
					break;

				// Ethernet
				case REG_ZZ_ETH_TX:
					ethernet_send_result = ethernet_send_frame(zdata);
					//printf("SEND frame sz: %ld res: %d\n",zdata,ethernet_send_result);
					break;
				case REG_ZZ_ETH_RX: {
					//printf("RECV eth frame sz: %ld\n",zdata);
					int frfb = ethernet_receive_frame();
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG4, frfb);
					break;
				}
				case REG_ZZ_ETH_MAC_HI: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[0] = (zdata & 0xff00) >> 8;
					mac[1] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_HI2: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[2] = (zdata & 0xff00) >> 8;
					mac[3] = (zdata & 0x00ff);
					break;
				}
				case REG_ZZ_ETH_MAC_LO: {
					uint8_t* mac = ethernet_get_mac_address_ptr();
					mac[4] = (zdata & 0xff00) >> 8;
					mac[5] = (zdata & 0x00ff);
					ethernet_update_mac_address();
					break;
				}
				case REG_ZZ_USBBLK_TX_HI: {
					usb_storage_write_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_TX_LO: {
					usb_storage_write_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_write_blocks(0, usb_storage_write_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] TX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USBBLK_RX_HI: {
					usb_storage_read_block = ((u32) zdata) << 16;
					break;
				}
				case REG_ZZ_USBBLK_RX_LO: {
					usb_storage_read_block |= zdata;
					if (usb_storage_available) {
						usb_status = zz_usb_read_blocks(0, usb_storage_read_block, usb_read_write_num_blocks, (void*)USB_BLOCK_STORAGE_ADDRESS);
					} else {
						printf("[USB] RX but no storage available!\n");
					}
					break;
				}
				case REG_ZZ_USB_STATUS: {
					//printf("[USB] write to status/blocknum register: %d\n", zdata);
					if (zdata==0) {
						// reset USB
						// FIXME memory leaks?
						//usb_storage_available = zz_usb_init();
					} else {
						// set number of blocks to read/write at once
						usb_read_write_num_blocks = zdata;
					}
					break;
				}
				case REG_ZZ_USB_BUFSEL: {
					// FIXME: obsolete!
					break;
				}
				case REG_ZZ_DEBUG: {
					//debug_lowlevel = zdata;
					break;
				}
				case REG_ZZ_DEBUG_TIMER: {
					audio_debug_timer(zdata);
					break;
				}
				case REG_ZZ_PRINT_CHR: {
					printf("%c",(int)(zdata&0xff));
					break;
				}
				case REG_ZZ_PRINT_HEX: {
					// print zdata has hex (follow up by \n via chr!)
					printf("%04x", (unsigned int)(zdata&0xffff));
					break;
				}
				case REG_ZZ_AUDIO_CONFIG: {
					// audio config
					audio_set_interrupt_enabled((int)(zdata & 1));
					break;
				}

				// ARM core 2 execution
				case REG_ZZ_ARM_RUN_HI:
					arm_run_address = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_RUN_LO:
					arm_run_address |= zdata;
					arm_app_run(arm_run_address);
					break;
				case REG_ZZ_ARM_ARGC:
					arm_run_env->argc = zdata;
					break;
				case REG_ZZ_ARM_ARGV0:
					arm_run_env->argv[0] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV1:
					arm_run_env->argv[0] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV2:
					arm_run_env->argv[1] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV3:
					arm_run_env->argv[1] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV4:
					arm_run_env->argv[2] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV5:
					arm_run_env->argv[2] |= zdata;
					break;
				case REG_ZZ_ARM_ARGV6:
					arm_run_env->argv[3] = ((u32) zdata) << 16;
					break;
				case REG_ZZ_ARM_ARGV7:
					arm_run_env->argv[3] |= zdata;
					break;
				case REG_ZZ_ARM_EV_CODE:
					arm_app_input_event(zdata);
					break;
				case REG_ZZ_AUDIO_SWAB:
					{
						int byteswap = 1;
						if (zdata&(1<<15)) byteswap = 0;
						audio_offset = (zdata&0x7fff)<<8; // *256
						audio_buffer_collision = audio_swab(audio_scale, audio_offset, byteswap);

						break;
					}
				case REG_ZZ_AUDIO_SCALE:
					audio_scale = zdata;
					break;
				case REG_ZZ_UNUSED_REG8C:
					// set up a test (set sleep time, and set counter to 0)
					zz_debug_test_ms = zdata;
					zz_debug_test_counter = 0;
					zz_debug_test_prev = 0;
					printf("[zzdebug] test reset, time: %lu\n", zz_debug_test_ms);
					break;

				case REG_ZZ_UNUSED_REG8E:
					// increase counter by one and compare with the number we are sent
					if (zdata > 0 && zz_debug_test_prev != zdata-1) {
						printf("[zzdebug] loss! zdata: %lu prev: %lu counter: %lu\n", zdata, zz_debug_test_prev, zz_debug_test_counter);
					}
					usleep(zz_debug_test_ms*1000);
					zz_debug_test_counter++;
					zz_debug_test_prev = zdata;
					break;

				case REG_ZZ_AUDIO_PARAM:
					printf("[REG_ZZ_AUDIO_PARAM] %lx\n", zdata);

					if (zdata<ZZ_NUM_AUDIO_PARAMS) {
						audio_param = zdata;
					} else {
						audio_param = 0;
					}
					break;
				case REG_ZZ_AUDIO_VAL:
					printf("[REG_ZZ_AUDIO_VAL] %lx\n", zdata);

					audio_params[audio_param] = zdata;
					if (audio_param == AP_TX_BUF_OFFS_LO) {
						uint8_t* addr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_TX_BUF_OFFS_HI]<<16)|audio_params[AP_TX_BUF_OFFS_LO]);
						if (((uint32_t)addr-(uint32_t)video_state->framebuffer)<0x100000*128) {
							audio_set_tx_buffer(addr);
							audio_request_init = 1;
						} else {
							printf("[audio] illegal tx address: 0x%p\n", addr);
						}
					} else if (audio_param == AP_RX_BUF_OFFS_LO) {
						uint8_t* addr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_RX_BUF_OFFS_HI]<<16)|audio_params[AP_RX_BUF_OFFS_LO]);
						if (((uint32_t)addr-(uint32_t)video_state->framebuffer)<0x100000*128) {
							audio_set_rx_buffer(addr);
							audio_request_init = 1;
						} else {
							printf("[audio] illegal tx address: 0x%p\n", addr);
						}
					} else if (audio_param == AP_DSP_UPLOAD) {
						uint8_t* program_ptr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_DSP_PROG_OFFS_HI]<<16)|audio_params[AP_DSP_PROG_OFFS_LO]);
						uint8_t* params_ptr = (uint8_t*)video_state->framebuffer +
								((audio_params[AP_DSP_PARAM_OFFS_HI]<<16)|audio_params[AP_DSP_PARAM_OFFS_LO]);

						if (zdata == 0) {
							printf("[audio] reprogramming from 0x%p and 0x%p\n", program_ptr, params_ptr);
							audio_program_adau(program_ptr, 5120);
							audio_program_adau_params(params_ptr, 4096);
						} else {
							printf("[audio] programming %ld params from 0x%p\n", zdata, params_ptr);
							audio_program_adau_params(params_ptr, zdata);
						}
					} else if (audio_param == AP_DSP_SET_LOWPASS) {
						// set lowpass filter params by cutoff freq (works only if default program is loaded!)
						audio_adau_set_lpf_params(zdata);
					} else if (audio_param == AP_DSP_SET_VOLUMES) {
						audio_adau_set_mixer_vol(zdata&0xff, (zdata>>8)&0xff);
					} else if (audio_param == AP_DSP_SET_PREFACTOR) {
						audio_adau_set_prefactor(zdata);
					} else if ((audio_param >= AP_DSP_SET_EQ_BAND1) && (audio_param <= AP_DSP_SET_EQ_BAND10)) {
						audio_adau_set_eq_gain(audio_param-AP_DSP_SET_EQ_BAND1, zdata);
					} else if (audio_param == AP_DSP_SET_STEREO_VOLUME) {
						audio_adau_set_vol_pan(zdata&0xff, (zdata>>8)&0xff);
					}
					break;
				case REG_ZZ_DECODER_PARAM:
					if (zdata<ZZ_NUM_DECODER_PARAMS) {
						decoder_param = zdata;
					} else {
						decoder_param = 0;
					}
					break;
				case REG_ZZ_DECODER_VAL:
					decoder_params[decoder_param] = zdata;
					break;
				case REG_ZZ_DECODER_FIFO:
					fifo_set_write_index(zdata);
					break;
				case REG_ZZ_DECODE:
					{
						// DECODER PARAMS:
						// 0: input buffer offset hi
						// 1: input buffer offset lo
						// 2: input buffer size hi
						// 3: input buffer size lo
						// 4: output buffer offset hi
						// 5: output buffer offset lo
						// 6: output buffer size hi
						// 7: output buffer size lo

						uint8_t* input_buffer = (uint8_t*)video_state->framebuffer
								+ ((decoder_params[0]<<16)|decoder_params[1]);
						size_t input_buffer_size = (decoder_params[2]<<16)|decoder_params[3];

						uint8_t* output_buffer = (uint8_t*)video_state->framebuffer
								+ ((decoder_params[4]<<16)|decoder_params[5]);
						size_t output_buffer_size = (decoder_params[6]<<16)|decoder_params[7];

						switch(zdata) {
							case DECODE_CLEAR:
								printf("[decode:clear]\n");
								fifo_clear();
							break;
							case DECODE_INIT:
								printf("[decode:mp3:%d] %p (%x) -> %p (%x)\n", (int)zdata, input_buffer, input_buffer_size,
										output_buffer, output_buffer_size);
								decode_mp3_init_fifo(input_buffer, input_buffer_size);
								decoder_bytes_decoded = -1;
							break;
							case DECODE_RUN: {
								int max_samples = output_buffer_size;
								int mp3_freq = mp3_get_hz();
								if (mp3_freq != 48000) {
									uint8_t* temp_buffer = output_buffer + AUDIO_TX_BUFFER_SIZE; // FIXME hack
									max_samples = mp3_get_hz()/50 * 2;
								
									decoder_bytes_decoded = decode_mp3_samples(temp_buffer, max_samples);
								
									// resample
									resample_s16((int16_t*)temp_buffer, (int16_t*)output_buffer,
											mp3_get_hz(), 48000, AUDIO_BYTES_PER_PERIOD / 4);
								
								} else {
									decoder_bytes_decoded = decode_mp3_samples(output_buffer, max_samples);
								}
							}
							break;
						}
						break;
					}
				}
			}

			// ack the write, set bit 31 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 31));
			need_req_ack = 1;
		} else if (readreq) {
			uint32_t zaddr = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG0);

			if (debug_lowlevel) {
				printf("READ: %08lx\n",zaddr);
			}
			u32 z3 = (zstate_raw & (1 << 25));

			if (zaddr >= MNT_FB_BASE || zaddr >= MNT_REG_BASE + 0x2000) {
				u8* ptr = (u8*) FRAMEBUFFER_ADDRESS;

				if (zaddr >= MNT_FB_BASE) {
					// read from framebuffer / generic memory
					ptr = ptr + zaddr - MNT_FB_BASE;
				} else if (zaddr < MNT_REG_BASE + 0x6000) {
					// 0x0000-0x1fff: read from ethernet RX frame
					// used by Z2
					ptr = (u8*) (ethernet_current_receive_ptr() + zaddr - (MNT_REG_BASE + 0x2000));
				} else if (zaddr < MNT_REG_BASE + 0x8000) {
					// 0x6000-0x7fff: boot ROM
					// used by Z2
					//printf("READ ROM: %08lx\n",zaddr);
					ptr = (u8*) (BOOT_ROM_ADDRESS + zaddr - (MNT_REG_BASE + 0x6000));
				} else if (zaddr < MNT_REG_BASE + 0xa000) {
					// 0x8000-0x9fff: read from TX frame (unusual)
					// FIXME: remove
					ptr = (u8*) (TX_FRAME_ADDRESS + zaddr - (MNT_REG_BASE + 0x8000));
				} else if (zaddr < MNT_REG_BASE + 0x10000) {
					// 0xa000-0xafff: read from block device (usb storage)
					// used by Z2
					ptr = (u8*) (USB_BLOCK_STORAGE_ADDRESS + zaddr - (MNT_REG_BASE + 0xa000));
				}

				if (z3) {
					u32 b1 = ptr[0] << 24;
					u32 b2 = ptr[1] << 16;
					u32 b3 = ptr[2] << 8;
					u32 b4 = ptr[3];
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1,
							b1 | b2 | b3 | b4);
				} else {
					if (zaddr >= MNT_REG_BASE + 0x6000 && zaddr < MNT_REG_BASE + 0x8000) {
						// autoboot rom
						u16 ubyte = ptr[0] << 8;
						u16 lbyte = ptr[1];
						//printf("READ ROM: [%04x]",ubyte|lbyte);
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, ubyte | lbyte);
					} else {
						u16 ubyte = ptr[0] << 8;
						u16 lbyte = ptr[1];
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, ubyte | lbyte);
					}
				}
			} else if (zaddr >= MNT_REG_BASE) {
				// read ARM "register"
				uint32_t data = 0;
				uint32_t zaddr32 = zaddr & 0xffffffc;

				//printf("REGR: %lx (%d)\n", zaddr, zaddr & 2);

				switch (zaddr32) {
					case REG_ZZ_VBLANK_STATUS:
						data = (zstate_raw & (1 << 21));
						break;
					case REG_ZZ_ARM_EV_SERIAL:
						data = arm_app_output_event();
						break;
					case REG_ZZ_ETH_MAC_HI: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[0] << 24 | mac[1] << 16 | mac[2] << 8 | mac[3];
						break;
					}
					case REG_ZZ_ETH_MAC_LO: {
						uint8_t* mac = ethernet_get_mac_address_ptr();
						data = mac[4] << 24 | mac[5] << 16;
						break;
					}
					case REG_ZZ_ETH_TX:
						// FIXME this is probably wrong (doesn't need swapping?)
						data = (ethernet_send_result & 0xff) << 24
								| (ethernet_send_result & 0xff00) << 16;
						break;
/* === XACP read registers === */
case 0x64: data = (uint32_t)x_status   << 16; break;
case 0xA8: data = (uint32_t)x_channels << 16; break;
case 0xAA: data = (uint32_t)x_error    << 16; break;
case 0xAC: data = (uint32_t)x_result   << 16; break;
case 0xAE: data = (uint32_t)x_rate     << 16; break;
case 0xF6: data = (uint32_t)x_debug_hi << 16; break;
case 0xF8: data = (uint32_t)x_debug_lo << 16; break;
/* === end XACP read registers === */
					case REG_ZZ_FW_VERSION:
						data = (REVISION_MAJOR << 24 | REVISION_MINOR << 16);
						break;
					case REG_ZZ_USB_STATUS:
						data = usb_status << 16;
						break;
					case REG_ZZ_USB_CAPACITY: {
						if (usb_storage_available) {
							printf("[USB] query capacity: %lx\n",zz_usb_storage_capacity(0));
							data = zz_usb_storage_capacity(0);
						} else {
							printf("[USB] query capacity: no device.\n");
							data = 0;
						}
						break;
					}
					case REG_ZZ_TEMPERATURE: {
						// includes REG_ZZ_VOLTAGE_AUX in lower 16 bits
						data = (((uint32_t)(xadc_get_temperature()*10.0)) << 16) | ((uint32_t)(xadc_get_aux_voltage()*100.0));
						break;
					}
					case REG_ZZ_VOLTAGE_INT: {
						data = ((int16_t)(xadc_get_int_voltage()*100.0)) << 16;
						break;
					}
					case REG_ZZ_CONFIG: {
						data = (amiga_interrupt_get())<<16;
						break;
					}
					case REG_ZZ_AUDIO_SWAB: {
						// misc status bits
						//printf("read 0x70: %d\n", audio_buffer_collision);
						data = (audio_buffer_collision)<<16 | fifo_get_read_index();
						break;
					}
					case REG_ZZ_AUDIO_CONFIG: {
						// is ZZ9000AX present?
						data = (adau_enabled)<<16;
						break;
					}
					case REG_ZZ_DECODER_VAL: {
						// used to determine if MP3 decoding has finished
						data = decoder_bytes_decoded;
						break;
					}
					case REG_ZZ_UNUSED_REG8C: {
						// sleep test for reads
						data = zz_debug_test_counter;
						break;
					}
				}

				if (z3) {
					mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
				} else {
					if (zaddr & 2) {
						// lower 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data);
					} else {
						// upper 16 bit
						mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG1, data >> 16);
					}
				}
			}

			// ack the read, set bit 30 in register 0
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, (1 << 30));
			need_req_ack = 2;
		} else {
			// there are no read/write requests, we can do other housekeeping
			idle_task_count++;

			if (idle_task_count > 10000000) {
				ethernet_task();
				idle_task_count=0;
			}

			if ((zstate & 0xff) == 0) {
				// RESET
				handle_amiga_reset();
			}

			if (audio_request_init) {
				audio_debug_timer(0);
				audio_init_i2s();
				audio_request_init = 0;
				audio_debug_timer(1);
			}

			/* === XACP deferred MP3 decode === */
			if (xacp_mp3_job_pending) {
				uint8_t      *xacp_raw = (uint8_t*)video_state->framebuffer + XACP_CMD_OFFSET;
				XACP_Command *xcmd     = (XACP_Command*)xacp_raw;
				uint32_t in_off  = be32(xcmd->input_offset);
				uint32_t in_size = be32(xcmd->input_size);
				uint32_t out_off = be32(xcmd->output_offset);
				uint32_t out_max = be32(xcmd->output_max_size);
				uint8_t *mp3_ptr = (uint8_t*)video_state->framebuffer + in_off;
				uint8_t *pcm_ptr = (uint8_t*)video_state->framebuffer + out_off;

				/* State variables persistent across idle passes */
				static uint8_t  *xacp_in_ptr;
				static uint32_t  xacp_in_rem;
				static uint32_t  xacp_bytes_written;
				static uint32_t  xacp_sample_rate;
				static uint32_t  xacp_channels;
				static mp3dec_t  xacp_dec;
				static uint32_t  xacp_frame_counter;

				/* Initialisation au premier appel */
				if (xacp_mp3_job_pending == 1) {
					xacp_mp3_job_pending = 2; /* en cours */
					/* Invalidate D-cache over MP3 input */
					{
						uintptr_t s = ((uintptr_t)mp3_ptr) & ~0x1FUL;
						uintptr_t e = ((uintptr_t)mp3_ptr + in_size + 31) & ~0x1FUL;
						Xil_DCacheInvalidateRange(s, (uint32_t)(e - s));
					}
					mp3dec_init(&xacp_dec);
					xacp_in_ptr       = mp3_ptr;
					xacp_in_rem       = in_size;
					xacp_bytes_written = 0;
					xacp_sample_rate  = 0;
					xacp_channels     = 0;
					xacp_frame_counter = 0;
				}

				/* Decoder 8 frames max par passe idle
				 * — laisse vivre Ethernet, USB, etc. */
				{
					/* static to avoid stack overflow on Cortex-A9 */
					static int16_t pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
					mp3dec_frame_info_t info;
					int frames_this_pass = 0;

					while (xacp_in_rem > 4 && frames_this_pass < 8) {
						int samples = mp3dec_decode_frame(&xacp_dec,
							xacp_in_ptr, (int)xacp_in_rem,
							pcm_buf, &info);

						if (info.frame_bytes == 0) {
							xacp_in_rem = 0; /* end of stream */
							break;
						}
						if (samples > 0) {
							uint32_t pcm_bytes = (uint32_t)(samples * info.channels * 2);
							if (xacp_bytes_written + pcm_bytes > out_max) {
								xacp_in_rem = 0;
								break;
							}
							memcpy(pcm_ptr + xacp_bytes_written, pcm_buf, pcm_bytes);
							xacp_bytes_written += pcm_bytes;
							xacp_sample_rate    = (uint32_t)info.hz;
							xacp_channels       = (uint32_t)info.channels;
						}
						xacp_in_ptr += info.frame_bytes;
						xacp_in_rem -= (uint32_t)info.frame_bytes;
						frames_this_pass++;
						xacp_frame_counter++;

						/* Heartbeat debug every 64 frames */
						if ((xacp_frame_counter & 63) == 0)
							x_debug_hi++;
					}
				}

				/* Decode termine ? */
				if (xacp_in_rem <= 4) {
					xacp_mp3_job_pending = 0;

					/* Flush PCM output */
					if (xacp_bytes_written > 0) {
						uintptr_t d = ((uintptr_t)pcm_ptr) & ~0x1FUL;
						uintptr_t e = ((uintptr_t)pcm_ptr + xacp_bytes_written + 31) & ~0x1FUL;
						Xil_DCacheFlushRange(d, (uint32_t)(e - d));
					}

					/* Write results to XACP command struct */
					xcmd->result1 = cpu_to_be32(xacp_bytes_written);
					xcmd->result2 = cpu_to_be32(xacp_sample_rate);
					xcmd->param1  = cpu_to_be32(xacp_channels);
					xcmd->error   = cpu_to_be32(0u);
					xcmd->status  = cpu_to_be32(2u); /* STATUS_DONE */

					/* Flush XACP command struct */
					{
						uintptr_t xs = ((uintptr_t)xacp_raw) & ~0x1FUL;
						Xil_DCacheFlushRange(xs, 128);
					}

					x_status = 2; /* STATUS_DONE via ZZ_RD(0x64) */
				}
			}
			/* ================================================================
			 * XACP streaming MP3 decode — XX14
			 *
			 * Changes vs XX10 :
			 *   1. MP3 ring (write/read) instead of sliding window
			 *      → no memmove on 68k side, no refill/ack protocol
			 *   2. Quantised decode : ~20ms PCM per ARM pass
			 *      max_samples = hz/50 * channels   (e.g. 44100/50*2 = 1764 samples)
			 *      → PCM ring stays LOW, natural pacing
			 *   3. 16KB staging buffer for MP3 ring wrap handling (minimp3 needs contiguous input)
			 * ================================================================ */
			if (xacp_stream_active) {
				uint8_t            *sc_raw = (uint8_t*)video_state->framebuffer + XACP_STREAM_OFFSET;
				XACP_StreamControl *sc     = (XACP_StreamControl*)sc_raw;

				/* Invalidate StreamControl — 160 bytes covers all fields */
				{
					uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
					Xil_DCacheInvalidateRange(xs, 160);
				}

				/* Lire les champs ring MP3 */
				uint32_t mp3_base  = be32(sc->mp3_base);
				uint32_t mp3_size  = be32(sc->mp3_size);
				uint32_t mp3_write = be32(sc->mp3_write);  /* [68k-owned] */
				uint32_t mp3_read  = be32(sc->mp3_read);   /* [ARM-owned] */
				uint32_t mp3_eof   = be32(sc->mp3_eof);

				/* Lire les champs ring PCM */
				uint32_t pcm_base  = be32(sc->pcm_base);
				uint32_t pcm_size  = be32(sc->pcm_size);
				uint32_t pcm_write = be32(sc->pcm_write);  /* [ARM-owned] */
				uint32_t pcm_read  = be32(sc->pcm_read);   /* [68k-owned] */

				uint8_t *mp3_buf      = (uint8_t*)video_state->framebuffer + mp3_base;
				uint8_t *pcm_buf_base = (uint8_t*)video_state->framebuffer + pcm_base;

				/* Bytes available in MP3 ring */
				uint32_t mp3_avail = (mp3_write >= mp3_read)
				                   ? (mp3_write - mp3_read)
				                   : (mp3_size  - mp3_read + mp3_write);

				/* PCM ring : free space and pending bytes */
				uint32_t pcm_free = (pcm_read + pcm_size - pcm_write - 1) % pcm_size;
				uint32_t pcm_used = pcm_size - 1 - pcm_free;

				/* Quantum AHI ~20ms (reference AX officiel : hz/50 * channels)
				 * Utilise sample_rate si deja connu, sinon 44100 par defaut.
				 * Limite le PCM produit par pass ARM → reste LOW, pacing naturel. */
				uint32_t q_sr = be32(sc->sample_rate);
				uint32_t q_ch = be32(sc->channels);
				if (q_sr == 0) q_sr = 44100u;
				if (q_ch == 0 || q_ch > 2u) q_ch = 2u;
				uint32_t pcm_quantum_bytes = (q_sr / 50u) * q_ch * 2u; /* PCM bytes per ~20ms quantum */

				/* BACKPRESSURE : ne pas decoder si PCM ring contient deja
				 * 16 quantums. Laisse le 68k drainer avant la prochaine passe. */
				if (pcm_used >= pcm_quantum_bytes * 16u) {
					goto xacp_stream_flush_sc;
				}

				if (mp3_avail > 4u) {
					/* ── Staging for MP3 ring wrap handling ────────────────
					 * minimp3 requires a contiguous input buffer. If the
					 * available data spans the ring end, copy both parts
					 * into a staging buffer. 16KB covers one quantum max. */
					static uint8_t  mp3_staging[16384];
					static int16_t  stream_pcm_buf[MINIMP3_MAX_SAMPLES_PER_FRAME];
					uint8_t        *decode_ptr;
					uint32_t        decode_len = mp3_avail;
					/* Cap at 16KB to avoid staging buffer overflow */
					if (decode_len > sizeof(mp3_staging))
						decode_len = sizeof(mp3_staging);

					uint32_t space_to_end = mp3_size - mp3_read;
					if (space_to_end >= decode_len) {
						/* Pas de wrap : acces direct */
						decode_ptr = mp3_buf + mp3_read;
						/* Invalidate only the relevant range */
						{
							uintptr_t s = ((uintptr_t)decode_ptr) & ~0x1FUL;
							uintptr_t e = ((uintptr_t)(decode_ptr + decode_len) + 31) & ~0x1FUL;
							Xil_DCacheInvalidateRange(s, (uint32_t)(e - s));
						}
					} else {
						/* Wrap : copier les deux parties dans staging */
						uint32_t part2 = decode_len - space_to_end;
						{
							uintptr_t s = ((uintptr_t)(mp3_buf + mp3_read)) & ~0x1FUL;
							uintptr_t e = ((uintptr_t)(mp3_buf + mp3_size) + 31) & ~0x1FUL;
							Xil_DCacheInvalidateRange(s, (uint32_t)(e - s));
						}
						{
							uintptr_t s = ((uintptr_t)mp3_buf) & ~0x1FUL;
							uintptr_t e = ((uintptr_t)(mp3_buf + part2) + 31) & ~0x1FUL;
							Xil_DCacheInvalidateRange(s, (uint32_t)(e - s));
						}
						memcpy(mp3_staging,                mp3_buf + mp3_read, space_to_end);
						memcpy(mp3_staging + space_to_end, mp3_buf,            part2);
						decode_ptr = mp3_staging;
					}

					/* ── Quantised decode loop ──────────────────────────────
					 * Stops when pcm_produced >= pcm_quantum_bytes (~20ms).
					 * Replaces the old unbounded "frames_this_pass < 8" limit. */
					mp3dec_frame_info_t info;
					uint32_t consumed_this_pass = 0;
					uint32_t pcm_produced       = 0;

					while (consumed_this_pass < decode_len &&
					       (decode_len - consumed_this_pass) > 4u &&
					       pcm_free > MINIMP3_MAX_SAMPLES_PER_FRAME * 4u &&
					       pcm_produced < pcm_quantum_bytes) {

						int samples = mp3dec_decode_frame(&xanxi_mp3d,
							decode_ptr + consumed_this_pass,
							(int)(decode_len - consumed_this_pass),
							stream_pcm_buf, &info);

						if (info.frame_bytes == 0) break;

						if (samples > 0) {
							uint32_t pcm_bytes = (uint32_t)(samples * info.channels * 2);

							/* Byte swap PCM : little-endian ARM → big-endian AHI */
							{
								uint16_t *p = (uint16_t*)stream_pcm_buf;
								uint32_t  n = pcm_bytes / 2u;
								uint32_t  k;
								for (k = 0; k < n; k++)
									p[k] = (p[k] >> 8) | (p[k] << 8);
							}

							/* Write decoded PCM to ring with wrap handling */
							uint32_t ste = pcm_size - pcm_write;
							if (pcm_bytes <= ste) {
								memcpy(pcm_buf_base + pcm_write, stream_pcm_buf, pcm_bytes);
								{
									uintptr_t d = ((uintptr_t)(pcm_buf_base + pcm_write)) & ~0x1FUL;
									uintptr_t e = ((uintptr_t)(pcm_buf_base + pcm_write + pcm_bytes) + 31) & ~0x1FUL;
									Xil_DCacheFlushRange(d, (uint32_t)(e - d));
								}
							} else {
								uint32_t p2 = pcm_bytes - ste;
								memcpy(pcm_buf_base + pcm_write, stream_pcm_buf, ste);
								{
									uintptr_t d = ((uintptr_t)(pcm_buf_base + pcm_write)) & ~0x1FUL;
									uintptr_t e = ((uintptr_t)(pcm_buf_base + pcm_size) + 31) & ~0x1FUL;
									Xil_DCacheFlushRange(d, (uint32_t)(e - d));
								}
								memcpy(pcm_buf_base, (uint8_t*)stream_pcm_buf + ste, p2);
								{
									uintptr_t d = ((uintptr_t)pcm_buf_base) & ~0x1FUL;
									uintptr_t e = ((uintptr_t)(pcm_buf_base + p2) + 31) & ~0x1FUL;
									Xil_DCacheFlushRange(d, (uint32_t)(e - d));
								}
							}

							pcm_write  = (pcm_write + pcm_bytes) % pcm_size;
							pcm_free   = (pcm_read + pcm_size - pcm_write - 1) % pcm_size;
							pcm_produced += pcm_bytes;

							/* Record sample_rate/channels from first decoded frame */
							if (be32(sc->sample_rate) == 0) {
								sc->sample_rate = cpu_to_be32((uint32_t)info.hz);
								sc->channels    = cpu_to_be32((uint32_t)info.channels);
							}
						}

						consumed_this_pass += (uint32_t)info.frame_bytes;
						sc->frames_decoded  = cpu_to_be32(be32(sc->frames_decoded) + 1u);
					}

					/* Update pcm_write and mp3_read ring pointers */
					sc->pcm_write = cpu_to_be32(pcm_write);
					mp3_read      = (mp3_read + consumed_this_pass) % mp3_size;
					sc->mp3_read  = cpu_to_be32(mp3_read);

					/* XX16 — mandatory intra-loop flush on Cortex-A9 :
					 * without this flush the 68k sees stale pcm_write
					 * and never dispatches the first AHI slot.
					 * Distinct from the final flush below (covers status/refill). */
					{
						uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
						Xil_DCacheFlushRange(xs, 128);
					}

					/* XX16 — first-advance debug via sc->flags (no UART available)
					 * flags[15:0]  = pcm_write at first advance
					 * flags[31:16] = frames_decoded at first advance
					 * 68k reads and displays these after STREAM_OPEN. */
					if (be32(sc->flags) == 0u && pcm_write > 0u) {
						sc->flags = cpu_to_be32(
						    ((be32(sc->frames_decoded) & 0xFFFFu) << 16) |
						    (pcm_write & 0xFFFFu));
					}

					/* LOW_WATER : < 32KB remaining in MP3 ring — signal 68k to refill */
					{
						uint32_t remaining_mp3 = (mp3_avail > consumed_this_pass)
						                        ? (mp3_avail - consumed_this_pass) : 0u;
						if (!mp3_eof && remaining_mp3 < 32768u) {
							sc->mp3_need_refill = cpu_to_be32(1u);
							sc->underrun_count  = cpu_to_be32(be32(sc->underrun_count) + 1u);
						}
					}

					/* EOF + MP3 ring empty → DECODE_DONE */
					{
						uint32_t rem = (mp3_avail > consumed_this_pass)
						             ? (mp3_avail - consumed_this_pass) : 0u;
						if (mp3_eof && rem <= 4u) {
							xacp_stream_active = 0;
							sc->status = cpu_to_be32(STREAM_DECODE_DONE);
						}
					}

				} else if (mp3_eof && mp3_avail <= 4u) {
					/* EOF and MP3 ring empty */
					xacp_stream_active = 0;
					sc->status = cpu_to_be32(STREAM_DECODE_DONE);
				}

				/* Final StreamControl flush — covers status, refill, underrun */
				xacp_stream_flush_sc:
				{
					uintptr_t xs = ((uintptr_t)sc_raw) & ~0x1FUL;
					Xil_DCacheFlushRange(xs, 160);
				}
			}
			/* === end XACP streaming XX16 === */

			// check for queued up ethernet frames and interrupt amiga
			if (interrupt_enabled_ethernet && ethernet_get_backlog()) {
				amiga_interrupt_set(AMIGA_INTERRUPT_ETH);
				eth_backlog_nag_counter = 0;
			}
		}

		// TODO: potential hang, timeout?
		if (need_req_ack) {
			while (1) {
				// 1. fpga needs to respond to flag bit 31 or 30 going high (signals request fulfilled)
				// 2. it does that by clearing the request bit
				// 3. we read register 3 until request bit (31:write, 30:read) goes to 0 again
				//
				u32 zstate = mntzorro_read(MNTZ_BASE_ADDR, MNTZORRO_REG3);
				u32 writereq = (zstate & (1 << 31));
				u32 readreq = (zstate & (1 << 30));
				if (need_req_ack == 1 && !writereq) // no more write request?
					break;
				if (need_req_ack == 2 && !readreq) // no more read request?
					break;
				if ((zstate & 0xff) == 0)
					break; // reset
			}
			mntzorro_write(MNTZ_BASE_ADDR, MNTZORRO_REG0, 0);
			need_req_ack = 0;
		}
	}

	cleanup_platform();
	return 0;
}
