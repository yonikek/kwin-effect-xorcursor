/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: *GPL-2.0-or-later */
#include "xorcursor.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"

namespace KWin {

    XorCursorEffect::XorCursorEffect()
    {
        // Both shakecursor and hidecursor are instantiated at KWin startup and
        // toggled active/inactive in place rather than being destroyed and
        // recreated, so a single lookup here is sufficient for the session. If
        // either is unloaded at runtime the QPointer nulls itself and
        // anotherEffectHidesCursor() gracefully returns false.
        refreshCursorHidingEffects();
        hideCursor();
    }

    XorCursorEffect::~XorCursorEffect()
    {
        showCursor();
    }

    void XorCursorEffect::refreshCursorHidingEffects()
    {
        m_shakeCursor = effects->findEffect(QStringLiteral("shakecursor"));
        m_hideCursor = effects->findEffect(QStringLiteral("hidecursor"));
    }

    bool XorCursorEffect::anotherEffectHidesCursor() const
    {
        // KWin reference-counts cursor hiding, so isCursorHidden() cannot tell us
        // whether a foreign effect has taken over the cursor: our own hideCursor()
        // call already makes it return true. Instead, query the two built-in
        // effects that render their own cursor and step aside while either is
        // active. Two QPointer null checks plus at most two virtual isActive()
        // calls, with no allocation and no iteration over unrelated effects.
        return (m_shakeCursor && m_shakeCursor->isActive())
        || (m_hideCursor && m_hideCursor->isActive());
    }

    GLTexture *XorCursorEffect::ensureCursorTexture()
    {
        if (!m_cursorTexture || m_cursorTextureDirty) {
            m_cursorTexture.reset();
            m_cursorTextureDirty = false;

            const auto cursor = effects->cursorImage();
            if (!cursor.image().isNull()) {
                QImage img = cursor.image().convertToFormat(QImage::Format_ARGB32_Premultiplied);
                m_cursorTexture = GLTexture::upload(img);
                if (!m_cursorTexture) {
                    return nullptr;
                }
                m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            }
        }
        return m_cursorTexture.get();
    }

    void XorCursorEffect::markCursorTextureDirty()
    {
        m_cursorTextureDirty = true;

        // The old cursor shape may have covered pixels the new one doesn't, so
        // damage the previous footprint. Without this, swapping from a large
        // cursor (e.g. a resize arrow) to a small one leaves stale inverted
        // pixels until something else happens to repaint them.
        if (!m_lastCursorRect.isNull()) {
            effects->addRepaint(Rect(m_lastCursorRect));
        }
    }

    void XorCursorEffect::showCursor()
    {
        if (m_isMouseHidden) {
            disconnect(effects, &EffectsHandler::cursorShapeChanged,
                       this, &XorCursorEffect::markCursorTextureDirty);
            disconnect(effects, &EffectsHandler::mouseChanged,
                       this, &XorCursorEffect::slotMouseChanged);
            effects->showCursor();
            m_cursorTexture.reset();
            m_lastCursorRect = QRect();
            m_isMouseHidden = false;
        }
    }

    void XorCursorEffect::hideCursor()
    {
        if (!m_isMouseHidden) {
            GLTexture *texture = nullptr;
            if (effects->isOpenGLCompositing()) {
                texture = ensureCursorTexture();
            }
            if (texture) {
                effects->hideCursor();
                connect(effects, &EffectsHandler::cursorShapeChanged,
                        this, &XorCursorEffect::markCursorTextureDirty);
                connect(effects, &EffectsHandler::mouseChanged,
                        this, &XorCursorEffect::slotMouseChanged);

                // Seed the damage tracker with the cursor's current footprint so the
                // first move erases the cursor's initial position correctly.
                const auto cursor = effects->cursorImage();
                const QSizeF cursorSize = QSizeF(cursor.image().size())
                / cursor.image().devicePixelRatio();
                m_lastCursorRect = QRectF(effects->cursorPos() - cursor.hotSpot(),
                                          cursorSize).toAlignedRect();
                                          m_isMouseHidden = true;
            }
        }
    }

    bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget,
                                      const RenderViewport &viewport,
                                      int mask,
                                      const Region &deviceRegion,
                                      LogicalOutput *screen)
    {
        // Paint the scene. KWin honours the passed deviceRegion exactly, so this
        // restores whatever the damaged region contained before we blend.
        if (!effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen)) {
            return false;
        }

        // Step aside while another effect owns the cursor. Skipping before the
        // restoration repaint avoids doing any work at all when we're not drawing.
        if (!m_isMouseHidden || anotherEffectHidesCursor()) {
            return true;
        }

        GLTexture *cursorTexture = ensureCursorTexture();
        if (!cursorTexture) {
            return true;
        }

        const auto cursor = effects->cursorImage();
        const QSizeF cursorSize = QSizeF(cursor.image().size())
        / cursor.image().devicePixelRatio();
        const QPointF p = effects->cursorPos() - cursor.hotSpot();
        const auto scale = viewport.scale();

        // Record the cursor's logical bounding box for the next move's damage.
        m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

        // Cursor region in device pixels. deviceRegion and the projection matrix
        // both work in device pixels, so no mixing of logical/device units here.
        const QRectF cursorDeviceRectF(p.x() * scale, p.y() * scale,
                                       cursorSize.width() * scale,
                                       cursorSize.height() * scale);
        const Region cursorRegion(Rect(cursorDeviceRectF.toAlignedRect()));
        if (cursorRegion.isEmpty()) {
            return true;
        }

        // KEY FIX — eliminate the double-inversion flicker:
        //
        // KWin may invoke paintScreen() more than once per frame for overlapping
        // pixels (one call per output, plus extra passes when a popup/menu layer
        // repaints over the cursor). The XOR blend below is destructive, so a
        // second blend would run on already-inverted pixels and cancel itself out.
        //
        // Re-painting the scene for just the cursor's region immediately before
        // every blend forces the framebuffer back to a clean, un-inverted copy of
        // the background. Every blend therefore starts from the same state, making
        // the whole effect idempotent regardless of how many paint passes KWin runs.
        if (!effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen)) {
            return false;
        }

        // Fixed-function XOR blend. The result for each pixel is:
        //   rgb = src.rgb * (1 - dst.rgb) + dst.rgb * (1 - src.alpha)
        //
        //  - Opaque cursor texel  (src = 1,1,1,1)   -> 1 - dst.rgb  = inverted
        //  - Transparent texel    (src = 0,0,0,0)   -> dst.rgb      = unchanged
        //  - Alpha is left untouched (srcAlpha = ZERO, dstAlpha = ONE).
        GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
        GLint previousSrcRgb = GL_ONE;
        GLint previousDstRgb = GL_ZERO;
        GLint previousSrcAlpha = GL_ONE;
        GLint previousDstAlpha = GL_ZERO;
        glGetIntegerv(GL_BLEND_SRC_RGB, &previousSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &previousDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &previousSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &previousDstAlpha);

        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ZERO, GL_ONE);

        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture
        | ShaderTrait::TransformColorspace);
        s->setColorspaceUniforms(ColorDescription::sRGB,
                                 renderTarget.colorDescription(),
                                 RenderingIntent::Perceptual);

        QMatrix4x4 mvp = viewport.projectionMatrix();
        mvp.translate(p.x() * scale, p.y() * scale);
        s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

        cursorTexture->render(cursorSize * scale);

        ShaderManager::instance()->popShader();

        glBlendFuncSeparate(previousSrcRgb, previousDstRgb,
                            previousSrcAlpha, previousDstAlpha);
        if (!blendWasEnabled) {
            glDisable(GL_BLEND);
        }

        return true;
    }

    bool XorCursorEffect::isActive() const
    {
        return m_isMouseHidden;
    }

    void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
    {
        if (pos == old) {
            return;
        }

        const auto cursor = effects->cursorImage();
        const QSizeF cursorSize = QSizeF(cursor.image().size())
        / cursor.image().devicePixelRatio();

        // Footprint of the cursor at its new position, in logical coordinates.
        const QRect newRect = QRectF(pos - cursor.hotSpot(), cursorSize).toAlignedRect();

        // Damage both the previous and the new footprint. Damaging only two small
        // rectangles instead of the whole screen is the repaint optimisation: a
        // mouse move now costs two cursor-sized repaints rather than a full-screen
        // one, while still erasing every stale inverted pixel along the path.
        effects->addRepaint(Rect(m_lastCursorRect));
        effects->addRepaint(Rect(newRect));
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
