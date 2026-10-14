/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/colorspace.h"
#include "effect/effect.h"
#include "opengl/glshadermanager.h"

namespace KWin
{

class GLFramebuffer;
class GLShader;
class GLTexture;

/**
 * Draws the mouse cursor by inverting the screen content underneath it, in
 * the spirit of Plasma's "Invert screen color" accessibility option.
 *
 * Historically this effect used the fixed-function GL_COLOR_LOGIC_OP with
 * GL_XOR to achieve an always-visible cursor. That approach is deprecated
 * (it does not exist at all in OpenGL ES and is awkward in modern core
 * profiles) and it only looks right when the cursor theme happens to be
 * white. This implementation instead renders the cursor with a small GLSL
 * fragment shader which samples the already-composited framebuffer, applies
 * `1.0 - color` to it (in linear light, taking the render target colorspace
 * into account) and masks the result with the cursor image's alpha channel.
 * Since only the alpha of the cursor texture is consumed, any cursor shape,
 * colour or theme works.
 */
class InvertCursorEffect : public Effect
{
    Q_OBJECT

public:
    InvertCursorEffect();
    ~InvertCursorEffect() override;

    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen) override;
    bool isActive() const override;

private Q_SLOTS:
    void slotMouseChanged(const QPointF &pos, const QPointF &old);

private:
    void showCursor();
    void hideCursor();
    GLTexture *ensureCursorTexture();
    void markCursorTextureDirty();
    /**
     * Lazily compiles the invert fragment shader (together with KWin's
     * standard transform vertex shader). Returns nullptr if the shader
     * could not be compiled.
     */
    GLShader *ensureShader();
    /**
     * Lazily creates the background grabber FBO that provides the composited
     * screen content as a texture to the invert shader. After this call,
     * m_grabberFbo is either valid for the requested @a size or null.
     */
    void ensureGrabber(const QSize &size);

    /**
     * Returns the bounding rectangle (in logical coordinates) of the
     * currently painted cursor image.
     */
    QRectF cursorRectLogical() const;

    std::unique_ptr<GLTexture> m_cursorTexture;
    // The grabber FBO does not own its color attachment, so the texture has
    // to outlive the framebuffer (both are reset together, FBO first).
    std::unique_ptr<GLTexture> m_grabberTexture;
    std::unique_ptr<GLFramebuffer> m_grabberFbo;
    std::unique_ptr<GLShader> m_shader;
    int m_backgroundSamplerLocation = -1;
    int m_cursorSamplerLocation = -1;
    QSize m_grabberSize;
    bool m_cursorTextureDirty = false;
    bool m_isMouseHidden = false;
    /** Bounding rect of the last painted cursor shape, in logical coordinates. */
    QRectF m_cursorPaintRect;
};

} // namespace KWin
