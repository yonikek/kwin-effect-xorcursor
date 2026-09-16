This version keeps the freeze-free offscreen-render architecture from the nosync version.

Changes relative to the previous cursor-mask version:
- removes fragment discard;
- outputs the color-managed inverted image with cursor alpha as coverage;
- explicitly composites with source-alpha blending, so the rectangular scratch target does not overwrite the screen;
- retains the persistent scratch framebuffer and Y-flipped cursor alpha sampling.

Target: KWin 6.7.5.
