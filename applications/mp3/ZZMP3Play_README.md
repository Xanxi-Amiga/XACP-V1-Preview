# ZZMP3Play

Current command-line MP3 player implementation for XACP V1.

Validated configuration:

* XX16c firmware
* MULTI8 buffering
* task priority 12

Current observed performance:

* Typical CPU usage around 12–15% on 68060 systems
* Stable playback achieved on long MP3 tracks
* Compatible with the current ARM-side MP3 streaming pipeline

Known limitations:

* Occasional bursts under heavy AmigaOS activity
* Replay/restart behavior still under refinement
* GUI frontend currently under development

This player represents the current recommended baseline implementation for XACP MP3 playback testing.
