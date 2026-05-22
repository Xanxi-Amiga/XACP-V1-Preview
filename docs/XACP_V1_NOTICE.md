# XACP V1 — Xanxi ARM Coprocessor Protocol
## Protocol Notice — Preview SDK Release

---

## Architecture Overview

XACP is a communication protocol between the Amiga 68k CPU and the ARM Cortex-A9
coprocessor on the ZZ9000 (Xilinx Zynq Z-7020 SoC).

The 68k triggers ARM services by writing opcodes to a dedicated Zorro III register.
The ARM processes requests either inline (fast ops) or deferred during its idle loop
(longer ops). Results are communicated via status registers or via shared DDR memory.

```
Amiga 68k                         ARM Cortex-A9 (Core0)
─────────────────                 ──────────────────────
Write opcode → REG_CMD  ───────►  Dispatch handler
Read status  ← REG_STATUS ◄──────  Set x_status / XACP_Command.status
Read/Write DDR ◄──────────────────► Shared framebuffer DDR region
```

---

## Shared DDR Memory

The ZZ9000 framebuffer DDR is directly accessible from both sides:

- **ARM** : via `video_state->framebuffer` (physical address, cached)
- **68k** : via Zorro III bus at `board + 0x00010000` (fb base)

All shared structures are placed at fixed offsets from the fb base:

| Region            | Offset from fb    | Size    | Purpose                  |
|-------------------|-------------------|---------|--------------------------|
| XACP_Command      | `0x04000000`      | 52 B    | Generic command struct   |
| XACP_StreamControl| `0x04002000`      | 68 B    | MP3 streaming state      |
| MP3 ring buffer   | `0x04100000`      | 512 KB  | Compressed MP3 input     |
| PCM ring buffer   | `0x04200000`      | 1 MB    | Decoded PCM output       |

---

## Endianness

The 68k is big-endian. The ARM is little-endian.

All fields in `XACP_Command` and `XACP_StreamControl` are stored **big-endian** in DDR.

- ARM writes with `cpu_to_be32(v)` before storing to shared memory.
- ARM reads with `be32(v)` after reading from shared memory.
- 68k reads and writes natively (no swap needed — it is big-endian).

**Do not remove these swaps. They are not optional.**

---

## Cache Coherence

The ARM Cortex-A9 has a write-back D-cache. Without explicit cache operations,
the 68k may see stale data.

Rules:
- Before reading DDR written by the 68k → `Xil_DCacheInvalidateRange()`
- After writing DDR for the 68k to read → `Xil_DCacheFlushRange()`
- Alignment: always align to 32-byte cache line boundaries before flush/invalidate.

---

## Ring Buffer Protocol

Both the MP3 input ring and PCM output ring follow the same model:

```
write pointer : owned by the producer (advances when data is written)
read pointer  : owned by the consumer (advances when data is consumed)

available = (write - read + size) % size
free      = (read + size - write - 1) % size
```

**Ownership:**

| Pointer     | Owner | Direction |
|-------------|-------|-----------|
| mp3_write   | 68k   | 68k → ARM |
| mp3_read    | ARM   | ARM → 68k |
| pcm_write   | ARM   | ARM → 68k |
| pcm_read    | 68k   | 68k → ARM |

---

## Opcode Map — XACP V1

### 0x0000–0x00FF : Core / System

| Opcode | Define          | Status      | Description              |
|--------|-----------------|-------------|--------------------------|
| 0x0001 | OP_MEMCPY       | Implemented | ARM DDR copy             |
| 0x0002 | OP_MANDELBROT   | Implemented | Mandelbrot/Julia render  |
| 0x0003 | OP_MP3_DECODE   | Implemented | Full-file MP3 decode     |
| 0x0004 | OP_STREAM_OPEN  | Implemented | MP3 ring streaming open  |
| 0x0005 | OP_STREAM_CLOSE | Implemented | MP3 ring streaming close |

### 0x0100–0x01FF : Stream / Audio / DSP

| Opcode | Define         | Status   | Description         |
|--------|----------------|----------|---------------------|
| 0x0110 | OP_SID_RENDER  | Reserved | ZZ SID synth        |
| 0x0120 | OP_MIDI_SF2    | Reserved | MIDI SF2 synth      |
| 0x0130 | OP_DSP_EQ      | Reserved | Software EQ         |
| 0x0140 | OP_PCM_RESAMPLE| Reserved | PCM resampling      |

### 0x0200–0x02FF : Image / RTG / Datatypes

| Opcode | Define              | Status   | Description         |
|--------|---------------------|----------|---------------------|
| 0x0200 | OP_IMG_SCALE        | Reserved | Image scaling       |
| 0x0210 | OP_CHUNKY_TO_PLANAR | Reserved | Chunky-to-planar    |
| 0x0211 | OP_PLANAR_TO_CHUNKY | Reserved | Planar-to-chunky    |
| 0x0220 | OP_DATATYPE_DECODE  | Reserved | Datatype decode     |

### 0x0300–0x03FF : Core1 / Execution

| Opcode | Define           | Status   | Description           |
|--------|------------------|----------|-----------------------|
| 0x0300 | OP_CORE1_START   | Reserved | Launch Core1 blob     |
| 0x0301 | OP_CORE1_STOP    | Reserved | Halt Core1            |
| 0x0302 | OP_CORE1_STATUS  | Reserved | Poll Core1 state      |
| 0x0310 | OP_PICODRIVE_LOAD| Reserved | PicoDrive ROM load    |
| 0x0320 | OP_DOSBOX_START  | Reserved | DOSBox instance       |
| 0x0330 | OP_MPEG_DECODE   | Reserved | MPEG video decode     |

### 0x0400–0x04FF : Compression / Archive

| Opcode | Define           | Status   | Description           |
|--------|------------------|----------|-----------------------|
| 0x0400 | OP_INFLATE       | Reserved | DEFLATE inflate       |
| 0x0410 | OP_LHA_EXTRACT   | Reserved | LHA extract           |
| 0x0420 | OP_LZX_DECOMPRESS| Reserved | LZX decompress        |

### 0x0500–0x05FF : Crypto / Hash

| Opcode | Define        | Status   | Description     |
|--------|---------------|----------|-----------------|
| 0x0500 | OP_HASH_MD5   | Reserved | MD5 hash        |
| 0x0501 | OP_HASH_SHA256| Reserved | SHA-256 hash    |
| 0x0510 | OP_AES_DECRYPT| Reserved | AES-128 decrypt |

**Reserved opcodes return `XACP_ERR_NOT_IMPLEMENTED (0xFFFF)` in `x_error`
with `x_status = XACP_STATUS_ERROR (3)`. No side effects.**

---

## XACP_Command Structure

Used for single-shot operations (OP_MEMCPY, OP_MP3_DECODE, etc.).

```c
typedef struct {
    uint32_t opcode;           /* command identifier          */
    uint32_t input_offset;     /* DDR offset of input data    */
    uint32_t input_size;       /* input data size in bytes    */
    uint32_t output_offset;    /* DDR offset for output       */
    uint32_t output_max_size;  /* max output size in bytes    */
    uint32_t param1;           /* command-specific parameter  */
    uint32_t param2;
    uint32_t param3;
    uint32_t param4;
    uint32_t result1;          /* command-specific result     */
    uint32_t result2;
    uint32_t status;           /* 0=idle 1=busy 2=done 3=err  */
    uint32_t error;            /* error code if status=3      */
} XACP_Command;
```

All fields big-endian in DDR.

---

## XACP_StreamControl Structure

Used for MP3 ring streaming (OP_STREAM_OPEN / OP_STREAM_CLOSE).

```c
typedef struct {
    uint32_t mp3_base;         /* MP3 ring offset in DDR (fixed) */
    uint32_t mp3_size;         /* MP3 ring total size            */
    uint32_t mp3_write;        /* [68k] write head               */
    uint32_t mp3_read;         /* [ARM] read head                */
    uint32_t mp3_need_refill;  /* [ARM→68k] 1 = low watermark    */
    uint32_t mp3_eof;          /* [68k] 1 = no more data         */
    uint32_t pcm_base;         /* PCM ring offset in DDR (fixed) */
    uint32_t pcm_size;         /* PCM ring total size            */
    uint32_t pcm_write;        /* [ARM] write head               */
    uint32_t pcm_read;         /* [68k] read head                */
    uint32_t sample_rate;      /* filled by ARM after 1st frame  */
    uint32_t channels;         /* filled by ARM after 1st frame  */
    uint32_t status;           /* IDLE/STREAMING/DONE/ERROR      */
    uint32_t error;            /* error code                     */
    uint32_t underrun_count;   /* incremented on low watermark   */
    uint32_t frames_decoded;   /* total MP3 frames decoded       */
    uint32_t flags;            /* debug: pcm_write_first [15:0]  */
                               /*        frames_first    [31:16] */
} XACP_StreamControl;
```

All fields big-endian in DDR.

---

## Currently Implemented Services

### MP3 Streaming (OP_STREAM_OPEN / OP_STREAM_CLOSE)

Real-time MP3 decode pipeline:

1. 68k writes MP3 data into the MP3 ring and advances `mp3_write`.
2. ARM decodes ~20ms quanta per idle pass via minimp3.
3. ARM writes decoded PCM (byte-swapped to big-endian) into the PCM ring.
4. 68k reads PCM and sends to AHI.
5. ARM signals `mp3_need_refill` at low watermark; 68k refills.
6. 68k sets `mp3_eof = 1` when file ends; ARM sets `status = DECODE_DONE`.

### MP3 Full-File Decode (OP_MP3_DECODE)

Deferred decode of a complete MP3 file already in DDR.
ARM processes 8 frames per idle pass to avoid blocking Zorro.
Results written to `XACP_Command.result1` (bytes), `result2` (sample rate).

### OP_MEMCPY

ARM-accelerated DDR-to-DDR copy with proper cache invalidate/flush.

### OP_MANDELBROT

Mandelbrot / Julia set renderer writing directly to the RTG framebuffer.

---

## Compatibility Philosophy

- The opcode values 1–5 are frozen and will not change.
- The `XACP_Command` and `XACP_StreamControl` layouts are frozen for V1.
- DDR offsets for ring buffers are fixed for V1.
- Reserved opcodes currently return `NOT_IMPLEMENTED` — no side effects.
- Future services will use opcode ranges ≥ 0x0100 as documented above.
- The protocol is designed to be forward-compatible:
  a client that sends an unknown opcode receives a clean error, not a crash.

---

## Zorro III Register Map (XACP-relevant)

| Register | Direction | Purpose                            |
|----------|-----------|------------------------------------|
| 0x62     | W         | Set input offset HI (legacy)       |
| 0x64     | R/W       | Status read / opcode write (REG_CMD)|
| 0x68     | W         | Set input size / param             |
| 0xA6     | W         | Set output offset                  |
| 0xA8     | R         | Read channels result               |
| 0xAA     | R         | Read error code                    |
| 0xAC     | R         | Read result                        |
| 0xAE     | R         | Read sample rate                   |
| 0xF6     | R         | Debug HI                           |
| 0xF8     | R         | Debug LO                           |

---

*XACP V1 — Preview SDK — Xanxi 2025*
*ZZ9000 firmware base: MNT 1.13 / BlitterStudio 2.0.1*
