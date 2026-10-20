# kwin-effect-xorcursor

XOR cursor effect for KDE Plasma

![IMG_20250527_210735](https://github.com/user-attachments/assets/3a3eeba0-4e56-4925-897c-e3607c03f8bf)

## Improvements over the master branch

- **Only the opaque parts of the cursor are inverted.** Master XORs the whole
  cursor image, so shadows and anti-aliased edges half-invert the desktop
  beneath them. Every pixel below the *Invert Threshold* (default: fully
  opaque) is now drawn the way the cursor theme made it, which keeps shadows
  and soft edges intact while still inverting the cursor body.
- **Colorspace correct, including HDR.** The inversion blends a mapped
  reference white (W - D) instead of logic-op XOR, so it works on SDR and
  float/HDR framebuffers alike; a clamp pass keeps over-bright pixels from
  going negative and is skipped on fixed-point buffers where it isn't needed.
- **Cheap repainting.** A mouse move repaints only the footprints the cursor
  leaves and enters; the area under the cursor is recomposed through the
  normal damage mechanism instead of re-rendering the whole scene (master
  repaints the entire screen on every mouse movement).
- **One draw call per pass.** All inverted regions of the cursor are batched
  into a single static vertex buffer per output scale, so the number of
  rectangles in the mask doesn't affect the number of draw calls.
- **Multi-monitor and fractional scale safe.** The mask is cached per output
  scale, rounded outward without overlaps, and outputs that don't contain the
  cursor are skipped entirely.
- **Handles cursor shape changes.** When the cursor image changes (resize
  handles, busy spinner, ...), the previous shape's footprint is invalidated,
  so no stale inverted pixels trail behind.
- **Plays well with other cursor effects.** The effect steps aside while
  *Shake Cursor* or *Hide Cursor* owns the pointer, instead of inverting
  whatever those effects just drew.
- **Configurable.** Settings (invert threshold, whether translucent parts are
  drawn as themed) are available from the configure button of the Desktop
  Effects module and apply immediately; they live in `kwinrc` under the
  `[Effect-xorcursor]` group.
- **Direct scanout friendly when idle.** With a cursor being drawn, direct
  scanout is deliberately blocked (it would bypass the compositing passes and
  the real cursor is hidden); with no cursor image there is nothing to draw
  and scanout is allowed again.

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```
