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

namespace
{
// GL_COLOR_LOGIC_OP operates on the fragment colour; alpha blending does not
// turn a partially transparent fragment into a partial XOR. For a real XOR
// cursor we therefore make the cursor a binary mask and use white as the XOR
// value. A half-alpha threshold keeps antialiased cursor edges from producing
// a noisy one-pixel fringe.
constexpr int s_xorAlphaThreshold = 128;

QImage createXorMask(const QImage &image)
{
    QImage mask = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    mask.setDevicePixelRatio(image.devicePixelRatio());

    for (int y = 0; y < mask.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(mask.scanLine(y));
        for (int x = 0; x < mask.width(); ++x) {
            const int alpha = qAlpha(line[x]);
            line[x] = alpha >= s_xorAlphaThreshold ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 255);
        }
    }

    return mask;
}
}

XorCursorEffect::XorCursorEffect()
{
    hideCursor();
}

XorCursorEffect::~XorCursorEffect()
{
    showCursor();
}

QRect XorCursorEffect::cursorRect() const
{
    const auto cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        return {};
    }

    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    return QRectF(effects->cursorPos() - cursor.hotSpot(), cursorSize).toAlignedRect();
}

GLTexture *XorCursorEffect::ensureCursorTexture()
{
    if (m_cursorTexture && !m_cursorTextureDirty) {
        return m_cursorTexture.get();
    }

    m_cursorTexture.reset();

    const auto cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        m_cursorTextureDirty = false;
        return nullptr;
    }

    const QImage mask = createXorMask(cursor.image());
    m_cursorTexture = GLTexture::upload(mask);
    if (!m_cursorTexture) {
        // Keep this dirty so a later paint can retry the upload.
        m_cursorTextureDirty = true;
        return nullptr;
    }

    m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    m_cursorTextureDirty = false;
    return m_cursorTexture.get();
}

void XorCursorEffect::updateCursorGeometry()
{
    const QRect oldRect = m_cursorRect;
    m_cursorRect = cursorRect();

    // The old cursor modified these pixels with XOR, so they must be
    // repainted before the cursor moves or changes shape. The new rectangle
    // must also be painted so the new XOR cursor is applied to fresh scene
    // pixels. Repaint even when the bounding rectangle is unchanged: a cursor
    // shape can change without changing its dimensions.
    const QRect damage = oldRect.united(m_cursorRect);
    if (!damage.isEmpty()) {
        effects->addRepaint(KWin::Rect(damage));
    }
}

void XorCursorEffect::slotCursorShapeChanged()
{
    m_cursorTextureDirty = true;
    updateCursorGeometry();
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::slotCursorShapeChanged);
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);

        effects->showCursor();
        m_cursorTexture.reset();
        m_cursorTextureDirty = false;
        m_cursorRect = {};
        m_isMouseHidden = false;
    }
}

void XorCursorEffect::hideCursor()
{
    if (m_isMouseHidden) {
        return;
    }

    if (!effects->isOpenGLCompositing()) {
        return;
    }

    if (!ensureCursorTexture()) {
        return;
    }

    effects->hideCursor();
    connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::slotCursorShapeChanged);
    connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);

    m_isMouseHidden = true;
    m_cursorRect = cursorRect();
    effects->addRepaint(KWin::Rect(m_cursorRect));
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (!m_isMouseHidden) {
        return;
    }

    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        // Do not leave the native cursor hidden if the replacement cannot be
        // rendered. The next cursor-shape notification will retry the upload.
        showCursor();
        return;
    }

    const QRect logicalCursorRect = cursorRect();
    if (logicalCursorRect.isEmpty()) {
        return;
    }

    // deviceRegion is in render-target coordinates. Map the cursor through
    // the viewport, including the output transform, before intersecting it
    // with the actual damage. This prevents XORing pixels outside the current
    // paint region.
    const Region cursorRenderRegion = viewport.mapToRenderTarget(Rect(logicalCursorRect)) & deviceRegion;
    if (cursorRenderRegion.isEmpty()) {
        return;
    }

    const auto cursor = effects->cursorImage();
    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF logicalPosition = effects->cursorPos() - cursor.hotSpot();

    // projectionMatrix() already contains the output transform. The model
    // translation therefore needs to be in the viewport's untransformed
    // device coordinate system, not global logical coordinates multiplied by
    // scale by hand.
    const QPointF devicePosition = viewport.mapToDeviceCoordinates(QRectF(logicalPosition, QSizeF())).topLeft();
    const QSizeF deviceSize = cursorSize * viewport.scale();

    GLboolean wasLogicOpEnabled = GL_FALSE;
    GLint previousLogicOp = GL_COPY;
    glGetBooleanv(GL_COLOR_LOGIC_OP, &wasLogicOpEnabled);
    glGetIntegerv(GL_LOGIC_OP_MODE, &previousLogicOp);

    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_XOR);

    auto shader = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture);
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(devicePosition.x(), devicePosition.y());
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    cursorTexture->render(cursorRenderRegion, deviceSize, true);
    ShaderManager::instance()->popShader();

    glLogicOp(previousLogicOp);
    if (!wasLogicOpEnabled) {
        glDisable(GL_COLOR_LOGIC_OP);
    }
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    Q_UNUSED(pos);

    if (pos != old) {
        updateCursorGeometry();
    }
}

} // namespace KWin

#include "moc_xorcursor.cpp"
