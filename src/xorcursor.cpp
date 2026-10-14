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
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
        // the XOR-composited pixels are still inverted on screen; repaint the
        // last painted cursor area so that the original content is restored
        // before KWin starts drawing its own cursor again
        if (!m_cursorPaintRect.isEmpty()) {
            const QRect r = m_cursorPaintRect.toAlignedRect();
            effects->addRepaint(r.x(), r.y(), r.width(), r.height());
        }
        // show the previously hidden mouse-pointer and free the loaded texture/picture.
        effects->showCursor();
        m_cursorTexture.reset();
        m_cursorPaintRect = QRectF();
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
            m_isMouseHidden = true;
            // force an initial paint of the cursor at its current position
            effects->addRepaintFull();
        }
    }
}

QRectF XorCursorEffect::cursorRectLogical() const
{
    const auto cursor = effects->cursorImage();
    const QSizeF size = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    return QRectF(p, size);
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }
    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return;
    }

    const QSizeF cursorSize = cursorRectLogical().size();
    const QPointF p = cursorRectLogical().topLeft();
    const auto scale = viewport.scale();

    // remember where we painted so that the same area can be restored later
    m_cursorPaintRect = QRectF(p, cursorSize);

    // Only paint if the cursor actually intersects the region being repainted
    // in this pass. Painting outside of the dirty region would leave the
    // XOR-composited state inconsistent (the previous inversion could not be
    // undone), which is exactly what this check prevents.
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
    if (!deviceRegion.intersects(Rect(cursorDeviceRect.toAlignedRect()))) {
        return;
    }

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
    if (pos == old) {
        return;
    }
    // Because the cursor is XOR-composited, both the area where the cursor was
    // previously painted and the area where it will be painted now have to be
    // repainted: the old one to restore the original pixels, the new one to
    // invert them. Repainting only these two rectangles instead of the whole
    // screen massively reduces the amount of work per frame.
    auto addLogicRectRepaint = [this](const QRectF &rect) {
        const QRect r = rect.toAlignedRect();
        effects->addRepaint(r.x(), r.y(), r.width(), r.height());
    };
    if (!m_cursorPaintRect.isEmpty()) {
        addLogicRectRepaint(m_cursorPaintRect);
    }
    addLogicRectRepaint(cursorRectLogical());
}

} // namespace KWin

#include "moc_xorcursor.cpp"
