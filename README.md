# KWin XOR cursor color-managed inversion refactor

This is the corrected version for KWin 6.7.5.

## Changes

- Removes `GL_COLOR_LOGIC_OP` / `glLogicOp(GL_XOR)`.
- Keeps the branch's repaint optimization (old cursor rect + new cursor rect).
- Uses a small reusable `GLFramebuffer`/`GLTexture` snapshot for the cursor area.
- Applies a shader pass using KWin's color-management helpers.
- The inversion transform now matches KWin 6.7.5's built-in `InvertEffect` shader, including the `saturation.glsl` path with saturation set to 1.0.
- Uses cursor alpha as a per-pixel inversion mask.
- Fixes KWin 6.7.5's `EffectsHandler::paintScreen()` being `void` and the C++ vexing-parse issue around `Region`.

## Applying

If you already applied the first patch, apply `0002-fix-kwin-6.7.5-build-errors.patch` on top of it.

Alternatively replace your `src/xorcursor.cpp` with the supplied corrected file.
