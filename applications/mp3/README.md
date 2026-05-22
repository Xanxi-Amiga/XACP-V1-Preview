# XACP MP3 Playback

This directory contains the first practical multimedia application developed on top of the XACP architecture.

The current implementation demonstrates:

* ARM-side MP3 decoding
* Shared DDR streaming
* AHI playback integration
* Streaming refill/backpressure handling
* Practical ARM compute offloading from the Amiga 68060

The XX16c firmware branch significantly reduced 68060 CPU usage compared to early implementations while improving playback stability.

Current work focuses on:

* GUI player integration
* AHI scheduling refinements
* Replay/restart handling
* mpega.library compatibility improvements
* Future DSP/equalizer support
