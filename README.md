# PVR DMA RENDERING 

Example of using KOS's `pvr_vertbuf_tail` / `pvr_vertbuf_written` API for DMA scene submission.

Uses SH4ZAM (a fast math library for the Sega Dreamcast's SH4 CPU) to perform all matrix math/transforms.

Provides a real-world tested implementation of near-z clipping.

Also shows off PVR fog.

Controls:
    analog stick up: move forward 
    analog stick down: move backward 
    analog stick left: turn left
    analog stick right: turn right
    dpad up: move vertically up
    dpad down: move vertically down
    dpad left: decrease PVR fog far-plane
    dpad right: increase PVR fog far-plane 
    L trigger: decrease PVR fog near-plane
    R trigger: increase PVR fog near-plane
    A button: disable texturing/enable debug vertex coloring
    Y button: reset scene to initial defaults

How to build:
- Compile/install KOS.
- Compile/install SH4zam, found at https://github.com/gyrovorbis/sh4zam .
- Clone kos-ports and install zlib, libjpeg, libpng and libkmg.
- Run `make`.