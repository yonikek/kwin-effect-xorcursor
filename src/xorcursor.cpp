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
        // Treat the entire old footprint as still inverted so the next frame
        // restores all of it, even where the new shape doesn't overlap.
        m_state += footprint;
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
        if (!m_state.isEmpty()) {
            addRepaintForRegion(m_state);
        }
        // show the previously hidden mouse-pointer again and free the loaded texture/picture.
        effects->showCursor();
        m_cursorTexture.reset();
        m_paintedRect = QRect();
        m_state = Region();
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
            m_state = Region();
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
    // Dirty exactly the affected pixels. KWin's damage bookkeeping translates
    // the global-scene-coordinate region into each output's local coordinates
    // and wakes up only the outputs that actually contain damaged pixels —
    // instead of recomposing the whole screen on every mouse move.
    effects->addRepaint(region);
}

bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // Damage that KWin recomposes on this output during this frame (global
    // scene coordinates). Everything inside it shows fresh scene content; our
    // XOR-inverted pixels within it are restored by the recomposition.
    const Region repainted = deviceRegion.translated(screen->geometry().topLeft());

    GLTexture *cursorTexture = m_isMouseHidden ? ensureCursorTexture() : nullptr;
    if (!cursorTexture) {
        return effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    }

    // Output-local, scaled geometry of the sprite quad (same coordinate space
    // the original effect used for its render position).
    const qreal scale = viewport.scale();
    const QRect spriteRect = spriteBoundingRectAt(effects->cursorPos());
    const QRectF localSprite(QPointF((spriteRect.x() - screen->geometry().x()) * scale,
                                     (spriteRect.y() - screen->geometry().y()) * scale),
                             QSizeF(spriteRect.width() * scale, spriteRect.height() * scale));

    // XOR bookkeeping (all regions in global scene coordinates):
    //  - pixels that are currently inverted AND were just recomposed have been
    //    restored to clean scene content by KWin -> they must be re-inverted;
    //  - pixels that are not inverted AND were just recomposed show clean
    //    scene content -> the new sprite footprint is drawn (XOR'd) onto them;
    //  - everything else keeps its current state untouched — crucially, the
    //    overlap between the old and new footprints stays inverted exactly
    //    once instead of being toggled off and back on.
    const Region paintedRegion = m_state + QRegion(m_paintedRect);
    const Region newFootprint(QRegion(spriteRect));
    const Region eraseRegion = paintedRegion.intersected(repainted);
    const Region drawRegion = newFootprint.subtracted(paintedRegion).intersected(repainted);

    // Decide *before* compositing whether any pixel's XOR state changes in
    // this frame. If nothing overlaps this output's damage, skip all GL work.
    const bool needsGlWork = !eraseRegion.isEmpty() || !drawRegion.isEmpty();

    if (!effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen)) {
        return false;
    }
    if (!needsGlWork) {
        return true;
    }

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

    // Clip every draw to the damaged pixels only, so undamaged screen content
    // is never touched. glScissor uses bottom-left origin.
    const int fbHeight = renderTarget.textureSize().height();
    const auto setScissorForRect = [fbHeight](const QRect &r) {
        glScissor(r.x(), fbHeight - r.y() - r.height(), r.width(), r.height());
    };
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    glEnable(GL_SCISSOR_TEST);

    // Restore (un-XOR) the old sprite wherever the scene was recomposed there.
    for (const QRect &r : eraseRegion) {
        const QRect local(int((r.x() - screen->geometry().x()) * scale),
                          int((r.y() - screen->geometry().y()) * scale),
                          qRound(r.width() * scale), qRound(r.height() * scale));
        setScissorForRect(local);
        cursorTexture->render(QSizeF(localSprite.width(), localSprite.height()));
    }
    // Invert (draw) the new sprite wherever fresh scene content appeared.
    for (const QRect &r : drawRegion) {
        const QRect local(int((r.x() - screen->geometry().x()) * scale),
                          int((r.y() - screen->geometry().y()) * scale),
                          qRound(r.width() * scale), qRound(r.height() * scale));
        setScissorForRect(local);
        cursorTexture->render(QSizeF(localSprite.width(), localSprite.height()));
    }

    shaderManager->popShader();

    if (!scissorWasEnabled) {
        glDisable(GL_SCISSOR_TEST);
    }
    glBlendFuncSeparate(blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha);
    if (!blendEnabled) {
        glDisable(GL_BLEND);
    }

    // Update the tracked set of inverted pixels: everything we just restored is
    // no longer inverted, everything we just drew is. Pixels whose XOR state
    // didn't change this frame (the untouched overlap between the old and the
    // new footprint, or anything outside this output's damage) keep their
    // previous entry in m_state.
    m_state -= eraseRegion;
    m_state += drawRegion;
    m_paintedRect = spriteRect;

    // Anything still inverted that hasn't been refreshed yet (e.g. because it
    // lies on another output that had no damage this frame) must keep being
    // repainted until it has been handled — otherwise a ghost cursor image
    // would linger.
    const Region pending = m_state.subtracted(newFootprint);
    if (!pending.isEmpty()) {
        addRepaintForRegion(pending);
    }

    return true;
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    Q_UNUSED(old)
    if (!m_isMouseHidden) {
        return;
    }

    const QRect newRect = spriteBoundingRectAt(pos);

    // Pixels whose visual state changes are exactly the symmetric difference
    // of the old and the new sprite footprint: the area being vacated must be
    // restored (un-XOR'd via recomposition) and the newly entered area must be
    // inverted. The overlapping interior keeps its inverted state and must NOT
    // be repainted — repainting it would toggle it back and make the cursor
    // disappear.
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

    addRepaintForRegion(dirty);
}

} // namespace KWin

#include "moc_xorcursor.cpp"
