/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"

#include <cmath>

namespace KWin
{

XorCursorEffect::XorCursorEffect()
{
    hideCursor();
}

XorCursorEffect::~XorCursorEffect()
{
    showCursor();
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
    // The cursor shape (and possibly its size) changed, so the sprite that is
    // currently XOR-inverted on screen no longer matches the theme image. The
    // whole painted footprint has to be refreshed: the next frame will restore
    // it and draw the new shape at the current pointer position.
    if (m_isMouseHidden && !m_paintedRect.isNull()) {
        const Region footprint(m_paintedRect);
        m_pendingRegion += footprint;
        addRepaintForRegion(footprint);
    }
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
        // Before handing rendering back to KWin's cursor plane, make sure all
        // pixels that are still XOR-inverted get restored by a final frame —
        // otherwise ghost cursor images would remain on screen.
        if (!m_pendingRegion.isEmpty()) {
            const Region remaining = m_pendingRegion;
            m_pendingRegion = Region();
            m_paintedRect = QRect();
            addRepaintForRegion(remaining);
        }
        // show the previously hidden mouse-pointer again and free the loaded texture/picture.
        effects->showCursor();
        m_cursorTexture.reset();
        m_paintedRect = QRect();
        m_pendingRegion = Region();
        m_isMouseHidden = false;
    }
}

void XorCursorEffect::hideCursor()
{
    if (!m_isMouseHidden) {
        // try to load the cursor-theme into a OpenGL texture and if successful then hide the mouse-pointer
        GLTexture *texture = nullptr;
        if (effects->isOpenGLCompositing()) {
            texture = ensureCursorTexture();
        }
        if (texture) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            m_paintedRect = QRect();
            m_pendingRegion = Region();
            m_isMouseHidden = true;
            // Draw the cursor for the first time — but request a repaint of
            // only the small rect it occupies instead of the full screen.
            slotMouseChanged(effects->cursorPos(), effects->cursorPos());
        }
    }
}

QRectF XorCursorEffect::cursorRectAt(const QPointF &hotspotPos, const QSize &textureSize) const
{
    const auto cursor = effects->cursorImage();
    const qreal dpr = std::max(1.0, double(cursor.image().devicePixelRatio()));
    const QSizeF logicalSize(double(textureSize.width()) / dpr, double(textureSize.height()) / dpr);
    return QRectF(hotspotPos - cursor.hotSpot(), logicalSize);
}

void XorCursorEffect::addRepaintForRegion(const Region &region)
{
    if (region.isEmpty()) {
        return;
    }
    // Translate the global-scene-coordinate region onto each output it
    // touches and dirty only those pixels. Outputs that don't intersect the
    // region are not woken up at all.
    for (LogicalOutput *output : workspace()->logicalOutputs()) {
        const Region local = region.translated(-output->globalGeometry().topLeft());
        if (local.intersects(QRect(QPoint(0, 0), output->logicalSize()))) {
            output->addRepaint(local);
        }
    }
}

bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    if (!effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen)) {
        return false;
    }
    if (!m_isMouseHidden) {
        return true;
    }
    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return true;
    }

    const auto cursor = effects->cursorImage();
    const QSize textureSize = cursorTexture->size();
    const QPointF p = effects->cursorPos();
    const QRectF spriteRect = cursorRectAt(p, textureSize);
    const auto scale = viewport.scale();

    // Map the global scene rect of the sprite into this output's framebuffer
    // (device pixel) coordinates. If it doesn't overlap the damage for this
    // frame there is nothing to do — skip all GL state changes entirely.
    const QPointF origin = screen->globalGeometry().topLeft();
    const QRectF localSprite(spriteRect.topLeft() - origin, spriteRect.size());
    const QRectF deviceSprite(QPointF(localSprite.x() * scale, localSprite.y() * scale),
                              QSizeF(localSprite.width() * scale, localSprite.height() * scale));
    const QRect deviceBounds = deviceSprite.toAlignedRect();
    if (deviceBounds.isNull() || !deviceRegion.intersects(deviceBounds)) {
        return true;
    }

    // Track the exact footprint of inverted pixels currently on screen so the
    // next mouse move can restore precisely those pixels. The region book-
    // keeping happens in global scene coordinates.
    const Region paintedRegion(spriteRect.toAlignedRect());
    m_paintedRect = spriteRect.toAlignedRect();
    m_pendingRegion -= paintedRegion;

    GLboolean blendEnabled = glIsEnabled(GL_BLEND);
    GLint blendSrcRgb = GL_ONE;
    GLint blendDstRgb = GL_ZERO;
    GLint blendSrcAlpha = GL_ONE;
    GLint blendDstAlpha = GL_ZERO;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

    // Restrict rasterization to the damage that actually intersects the
    // cursor sprite. This both saves fill-rate and guarantees we never
    // re-XOR pixels outside the tracked dirty regions (which would visibly
    // toggle them back to their un-inverted state).
    QRegion clip = deviceRegion.intersected(QRegion(deviceBounds));
    glScissor(clip.boundingRect().x(), clip.boundingRect().y(),
              clip.boundingRect().width(), clip.boundingRect().height());
    glEnable(GL_SCISSOR_TEST);

    auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture);
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate((spriteRect.x() - origin.x()) * scale, (spriteRect.y() - origin.y()) * scale);
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    cursorTexture->render(QSizeF(deviceBounds.size()));
    ShaderManager::instance()->popShader();

    glDisable(GL_SCISSOR_TEST);
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    if (!blendEnabled) {
        glDisable(GL_BLEND);
    }

    // If part of our tracked dirty set lies outside this output or wasn't
    // covered (e.g. clipped away), keep requesting repaints until everything
    // has been refreshed.
    if (!m_pendingRegion.isEmpty()) {
        addRepaintForRegion(m_pendingRegion);
    }

    return true;
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (!m_isMouseHidden) {
        return;
    }

    const QSize textureSize = m_cursorTexture ? m_cursorTexture->size()
                                              : effects->cursorImage().image().size();
    const QRect newRect = cursorRectAt(pos, textureSize).toAlignedRect();

    // Pixels whose visual state changes are exactly the symmetric difference
    // of the old and the new sprite footprint: the area being vacated must be
    // restored (un-XOR'd) and the newly entered area must be inverted. The
    // overlapping interior keeps its inverted state and must NOT be touched —
    // repainting it would toggle it back and make the cursor disappear.
    const QRect oldRect = m_paintedRect.isNull()
        ? cursorRectAt(old, textureSize).toAlignedRect()
        : m_paintedRect;

    Region dirty;
    if (m_paintedRect.isNull()) {
        // Nothing has been painted yet (first paint / re-activation): only the
        // new footprint needs to be refreshed, restoring happens implicitly.
        dirty += newRect;
    } else {
        dirty += oldRect;
        dirty += newRect;
        dirty -= oldRect.intersected(newRect);
    }

    // Anything still pending from earlier moves/shape changes in this frame
    // also needs to be refreshed.
    dirty += m_pendingRegion;

    m_pendingRegion = dirty;
    addRepaintForRegion(dirty);
}

} // namespace KWin

#include "moc_xorcursor.cpp"
