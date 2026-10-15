/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"

#include <KWindowSystem/KX11Extras>

#include <cmath>

Q_DECLARE_METATYPE(KWin::Region) // the Region type in this KWin does not carry Q_DECLARE_METATYPE

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
        const Region footprint(QRegion(m_paintedRect));
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

QRect XorCursorEffect::spriteBoundingRectAt(const QPointF &hotspotPos) const
{
    // Device-pixel-aligned bounding box (global scene coordinates) of the
    // cursor sprite when its hotspot is at |hotspotPos|. Rounded outwards so
    // the rectangle always fully covers the drawn quad.
    const auto cursor = effects->cursorImage();
    const QSizeF logicalSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF topLeft = hotspotPos - cursor.hotSpot();
    return QRectF(topLeft, logicalSize).toAlignedRect();
}

void XorCursorEffect::addRepaintForRegion(const Region &region)
{
    if (region.isEmpty()) {
        return;
    }
    // Dirty exactly the affected pixels. In the X11 backend these calls travel
    // through the _KWIN_DIRECT_PAINT mechanism straight into the per-output
    // damage bookkeeping, waking up only the outputs that actually contain
    // damaged pixels (a region spanning several screens marks each of them
    // separately, already translated into their local coordinates). On Wayland
    // sessions without KX11Extras there is no public per-region repaint API,
    // so fall back to a full repaint to keep the cursor artifact-free.
    if (KX11Extras *x11 = KX11Extras::instance()) {
        x11->addRepaint(region);
    } else {
        effects->addRepaintFull();
    }
}

bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // Fold any queued cursor damage into this output's dirty region *before*
    // compositing, so KWin recomposes precisely the pixels whose inverted XOR
    // state must change — instead of the whole screen as before.
    Region outputDamage = deviceRegion;
    if (m_isMouseHidden && !m_pendingRegion.isEmpty()) {
        outputDamage += m_pendingRegion.translated(-screen->geometry().topLeft());
    }

    // Decide *before* compositing whether the cursor sprite intersects this
    // output's damage. If it doesn't, no pixel's XOR state changes in this
    // frame, so we can skip all GL work entirely.
    GLTexture *cursorTexture = m_isMouseHidden ? ensureCursorTexture() : nullptr;
    bool overlapsDamage = false;
    QRectF localSprite;
    const qreal scale = viewport.scale();
    if (cursorTexture) {
        const QRect spriteRect = spriteBoundingRectAt(effects->cursorPos());
        localSprite = QRectF(QPointF((spriteRect.x() - screen->geometry().x()) * scale,
                                     (spriteRect.y() - screen->geometry().y()) * scale),
                             QSizeF(spriteRect.width() * scale, spriteRect.height() * scale));
        overlapsDamage = outputDamage.intersects(localSprite.toAlignedRect());
    }

    if (!effects->paintScreen(renderTarget, viewport, mask, outputDamage, screen)) {
        return false;
    }
    if (!m_isMouseHidden || !cursorTexture || !overlapsDamage) {
        return true;
    }

    // Track the exact footprint of inverted pixels currently on screen so the
    // next mouse move can restore precisely those pixels. Bookkeeping happens
    // in global scene coordinates; the render position below stays in the
    // output-local, scaled coordinates used by the original effect.
    const QRect spriteRect = spriteBoundingRectAt(effects->cursorPos());
    m_paintedRect = spriteRect;
    m_pendingRegion -= Region(QRegion(spriteRect));

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

    ShaderManager *shaderManager = ShaderManager::instance();
    // pushShader()/popShader() manage the shader's lifetime internally: they
    // return a raw GLShader* and popShader() may delete it, so keep it as a
    // plain pointer and always pair the call with popShader().
    GLShader *s = shaderManager->pushShader(ShaderTrait::MapTexture);
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(localSprite.x(), localSprite.y());
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    cursorTexture->render(QSizeF(localSprite.width(), localSprite.height()));
    shaderManager->popShader();

    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    if (!blendEnabled) {
        glDisable(GL_BLEND);
    }

    // If part of our tracked dirty set lies outside this output or wasn't
    // covered by this frame, keep requesting repaints until everything has
    // been refreshed.
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

    const QRect newRect = spriteBoundingRectAt(pos);

    // Pixels whose visual state changes are exactly the symmetric difference
    // of the old and the new sprite footprint: the area being vacated must be
    // restored (un-XOR'd) and the newly entered area must be inverted. The
    // overlapping interior keeps its inverted state and must NOT be touched —
    // repainting it would toggle it back and make the cursor disappear.
    Region dirty;
    if (m_paintedRect.isNull()) {
        // Nothing has been painted yet (first paint / re-activation): only the
        // new footprint needs to be refreshed, restoring happens implicitly.
        dirty += Region(QRegion(newRect));
    } else {
        const QRect oldRect = m_paintedRect;
        const QRect intersection = oldRect.intersected(newRect);
        dirty += Region(QRegion(oldRect.subtracted(intersection)));
        dirty += Region(QRegion(newRect.subtracted(intersection)));
    }

    // Anything still pending from earlier moves/shape changes in this frame
    // also needs to be refreshed.
    dirty += m_pendingRegion;

    m_pendingRegion = dirty;
    addRepaintForRegion(dirty);
}

} // namespace KWin

#include "moc_xorcursor.cpp"
