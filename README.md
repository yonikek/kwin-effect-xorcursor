# KWin XOR cursor refactor

This is a proposed refactor for `yonikek/kwin-effect-xorcursor` on the
`repaint-optimisation` branch.

## What changes

- Removes `GL_COLOR_LOGIC_OP` / `glLogicOp(GL_XOR)` from the cursor paint path.
- Keeps the existing repaint optimization (old cursor rect + new cursor rect).
- Paints the affected cursor region once, snapshots that region into a small
  reusable framebuffer texture, and applies a shader pass over the snapshot.
- Uses KWin's color-management helpers and the same gamma-2.2 inversion math
  used by the built-in accessibility `InvertEffect`.
- Uses the cursor alpha as the inversion mask, preserving antialiased cursor
  edges instead of relying on integer framebuffer XOR.

## Files

- `xorcursor.cpp` — proposed implementation
- `xorcursor.h` — matching declarations
- `0001-use-color-managed-inversion-instead-of-gl-logic-op.patch` — unified diff

## Notes

The patch has not been compiled in this environment because the KWin/Qt/KF6
build dependencies are not installed here. The most important integration point
to verify in a real KWin development environment is the custom shader's
`colormanagement.glsl` include, since that is resolved by KWin's shader
preprocessor.
