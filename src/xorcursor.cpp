/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"

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
            m_cursorTexture = GLTexture::upload(cursor.image());
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
    // A cursor shape change can change both the pixels and the cursor bounds.
    // Repaint the whole output rather than trying to guess which part of the
    // old cursor needs restoring. This is deliberately conservative: XOR is
    // stateful, so missing even one old cursor pixel can leave an artefact.
    effects->addRepaintFull();
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
        effects->showCursor();
        m_cursorTexture.reset();
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
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
            effects->addRepaintFull();
        }
    }
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget,
                                  const RenderViewport &viewport,
                                  int mask,
                                  const Region &deviceRegion,
                                  LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }

    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        // Never leave the real cursor hidden if our replacement cannot be
        // rendered.
        showCursor();
        return;
    }

    const auto cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        showCursor();
        return;
    }

    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    const auto scale = viewport.scale();

    // Keep the original rendering path intact. In particular, don't map the
    // cursor quad through RenderViewport here: projectionMatrix() and the
    // existing KWin cursor coordinates already match the effect's rendering
    // path. The previous optimisation mixed logical and render-target regions,
    // which caused the XOR quad to be clipped/misaligned and made it flicker.
    Region cursorRegion = Region(Rect(QRectF(p, cursorSize).toAlignedRect()));
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_XOR);

    auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
    s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    cursorTexture->render(cursorSize * scale);

    ShaderManager::instance()->popShader();
    glDisable(GL_COLOR_LOGIC_OP);
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    m_cursorPoint = pos.toPoint();
    if (pos != old) {
        // This is intentionally full-screen. XOR modifies the framebuffer,
        // so repainting only the old/new cursor rectangles is unsafe if KWin's
        // damage tracking or another effect changes pixels underneath it.
        effects->addRepaintFull();
    }
}

} // namespace KWin
#include "moc_xorcursor.cpp"
