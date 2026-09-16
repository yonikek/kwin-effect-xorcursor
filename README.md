# xorcursor KWin 6.7.5 performance refactor

Complete source files for the color-managed cursor inversion implementation.

Changes from the previous working version:
- scratch texture/FBO allocations are retained while the cursor moves;
- the scratch texture can be reused when the current cursor is smaller than the largest one seen;
- the common, untransformed framebuffer case uses `glCopyTexSubImage2D()` directly;
- transformed/unsupported cases fall back to `GLFramebuffer::blitFromRenderTarget()`;
- the working cursor-mask Y flip is retained.

Target API: KWin 6.7.5.
