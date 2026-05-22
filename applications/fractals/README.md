# XACP Fractal Rendering

These Mandelbrot rendering demos were among the earliest public demonstrations of the XACP architecture on real ZZ9000 hardware.

The first public demonstrations were shown on:

* Amibay forums
* YouTube videos
* May 7th, 2026

At the time, these demos validated several important XACP concepts:

* ARM-side Mandelbrot rendering
* Shared DDR communication between 68k and ARM
* RTG framebuffer rendering
* Offscreen rendering pipelines
* Progressive rendering
* Core1 runtime experimentation
* ARM compute offloading from the Amiga 68060

The original demonstrations included:

* 68060 vs ARM Mandelbrot comparisons
* ARM-rendered zoom sequences
* Real-time rendering experiments
* Early framebuffer and mailbox tests

These demos were developed during the rapid evolution of the XACP protocol and firmware architecture. Several internal command structures and framebuffer conventions changed over time between the early Mandelbrot branches and the current XX16c firmware generation.

As a result, some historical Mandelbrot demo clients may no longer work correctly without adaptation to the current XACP V1 command structures and framebuffer handling rules.

The original demonstrations nevertheless remain historically important because they represent some of the first successful public demonstrations of generic ARM compute offloading on the ZZ9000 platform.

