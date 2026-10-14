# kwin-effect-xorcursor

Invert cursor effect for KDE Plasma

![IMG_20250527_210735](https://github.com/user-attachments/assets/3a3eeba0-4e56-4925-897c-e3607c03f8bf)

## How it works

The cursor is no longer composited with the deprecated fixed-function
`GL_COLOR_LOGIC_OP`/`GL_XOR` path. Instead, after the screen has been
composited, the framebuffer content is grabbed into a texture and a small
GLSL fragment shader (`src/shaders/cursor_invert.fragment`) draws the cursor
by inverting the pixels underneath it — the same `1.0 - color` operation
that Plasma's "Invert screen color" accessibility option performs. The
cursor image's alpha channel is used as a coverage mask (anti-aliased edges
invert partially, transparent areas leave the background untouched), so the
inversion result does not depend on the cursor's RGB values at all.

**Any cursor theme and colour works** — white, black, red, Breeze, Bibata,
whatever you like; there is no more "use a white cursor theme" caveat.

## Build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```
