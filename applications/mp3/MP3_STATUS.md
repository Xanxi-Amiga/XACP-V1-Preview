# XACP MP3 Streaming Status

Current MP3 streaming implementation for XACP V1.

Current firmware branch:

* XX16c

Current validated features:

* ARM-side MP3 decoding using minimp3
* Shared DDR MP3 ring buffer
* Shared DDR PCM ring buffer
* AHI playback from the Amiga side
* Streaming refill/backpressure system
* MP3 playback through AmigaAMP
* Partial mpega.library compatibility

Current observations:

* Stable playback achieved with MULTI8 buffering
* CPU usage typically around 12–15% on 68060 systems
* Lower MULTI values increase CPU usage
* Higher MULTI values may introduce underruns/glitches
* Direct no-copy PCM experiments reduced CPU usage further but introduced instability

Known limitations:

* Occasional audio glitches under heavy AmigaOS activity
* Replay/restart handling still incomplete
* Some builds remain under evaluation
* AHI task scheduling/priorities still under investigation

Current state:
This implementation is already usable for daily testing and real playback usage, although some areas are still being refined.

