# mpega.library for XACP XX16c BETA

ARM-assisted MPEG audio playback library for the ZZ9000 XACP environment.

This beta release provides hardware-assisted MP3 decoding using the ARM side of the ZZ9000 while maintaining compatibility with several classic Amiga players through the standard `mpega.library` API.

Current implementation status:

* ARM/XACP decoding pipeline operational
* Stable playback confirmed
* Replay and long playback sessions working
* Reduced 68k CPU usage compared to software decoding
* Compatible with legacy Amiga software using `mpega.library`

## Player compatibility

### SongPlayer

Status: supported / working well

SongPlayer currently provides the best compatibility among tested third-party players using the legacy `mpega.library` interface.

Playback, seeking and timing are functional and stable in current usage.

### AmigaAMP

Status: working with cosmetic issues

Audio playback works correctly.

However, some metadata and UI elements are currently incomplete or inaccurate through the legacy `mpega.library` ABI layer, including:

* total duration display
* bitrate display
* slider/timeline behavior

This appears related to subtle historical ABI expectations specific to AmigaAMP rather than to the XACP decoding engine itself.

Current implementation nevertheless allows practical day-to-day playback usage with AmigaAMP.

### HippoPlayer

Status: not recommended for now

HippoPlayer currently exhibits unstable behavior and performance spikes with the asynchronous XACP streaming model.

Compatibility work may continue later, but HippoPlayer is not considered supported in this beta.

## Reference player

The reference player for XACP playback currently remains `ZZPlayGUI`.

Unlike legacy players relying on `mpega.library`, ZZPlayGUI communicates directly with the XACP streaming backend and implements several XACP-specific optimizations and buffering strategies visible in the source code.

This allows:

* smoother playback
* lower CPU spikes
* reduced refill overhead
* better synchronization with the asynchronous ARM decoding pipeline

Best overall playback experience is currently achieved through the native ZZPlayGUI/XACP playback path.

## Notes

This beta release already allows current practical usage with the players listed above while continuing to refine compatibility and ABI behavior across the legacy Amiga software ecosystem.

The main objective of this release is to provide:

* ARM-assisted decoding
* XACP streaming architecture
* legacy player compatibility
* low-overhead playback on classic Amiga systems

Further ABI refinements and compatibility improvements may arrive in future versions.

This implementation was written from scratch for the XACP/ZZ9000 environment using a standard GCC/libnix toolchain and does not derive from the original mpega.library source code.

