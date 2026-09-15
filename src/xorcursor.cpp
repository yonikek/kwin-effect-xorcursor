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

constexpr int s_xorAlphaThreshold = 128;

/*
 * GL_COLOR_LOGIC_OP does not implement alpha compositing. For this effect we
 * want a cursor to mean "invert this pixel", not "XOR with the cursor's RGB
 * value". Convert the cursor to a binary white mask:
 *
 *   active pixel   -> 0xffffffff
 *   inactive pixel -> 0x00000000
 *
 * The RGB values are what matter to the XOR operation; the alpha channel is
 * masked off while drawing below.
 */
QImage createXorMask(const QImage &image)
{
    QImage mask = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    mask.setDevicePixelRatio(image.devicePixelRatio());

    for (int y = 0; y < mask.height(); ++y) {
        auto *line = reinterpret_cast<QRgb *>(mask.scanLine(y));
        for (int x = 0; x < mask.width(); ++x) {
            line[x] = qAlpha(line[x]) >= s_xorAlphaThreshold
                ? qRgba(255, 255, 255, 255)
                : qRgba(0, 0, 0, 255);
        }
    }

    return mask;
}

} // namespace

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

    const QSizeF cursorSize =
        QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();

    return QRectF(effects->cursorPos() - cursor.hotSpot(), cursorSize)
        .toAlignedRect();
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
        m_cursorTextureDirty = true;
        return nullptr;
    }

    m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    m_cursorTexture->setFilter(GL_NEAREST);
    m_cursorTextureDirty = false;

    return m_cursorTexture.get();
}

void XorCursorEffect::updateCursorGeometry()
{
    const QRect oldRect = m_cursorRect;
    const QRect newRect = cursorRect();

    m_cursorRect = newRect;

    const QRect damage = oldRect.united(newRect);
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
    if (!m_isMouseHidden) {
        return;
    }

    disconnect(effects,
               &EffectsHandler::cursorShapeChanged,
               this,
               &XorCursorEffect::slotCursorShapeChanged);
    disconnect(effects,
               &EffectsHandler::mouseChanged,
               this,
               &XorCursorEffect::slotMouseChanged);

    effects->showCursor();
    m_cursorTexture.reset();
    m_cursorTextureDirty = false;
    m_cursorRect = {};
    m_isMouseHidden = false;
}

void XorCursorEffect::hideCursor()
{
    if (m_isMouseHidden || !effects->isOpenGLCompositing()) {
        return;
    }

    if (!ensureCursorTexture()) {
        return;
    }

    effects->hideCursor();

    connect(effects,
            &EffectsHandler::cursorShapeChanged,
            this,
            &XorCursorEffect::slotCursorShapeChanged);
    connect(effects,
            &EffectsHandler::mouseChanged,
            this,
            &XorCursorEffect::slotMouseChanged);

    m_isMouseHidden = true;
    m_cursorRect = cursorRect();

    if (!m_cursorRect.isEmpty()) {
        effects->addRepaint(KWin::Rect(m_cursorRect));
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
        showCursor();
        return;
    }

    const auto cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        showCursor();
        return;
    }

    const QSizeF cursorSize =
        QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF logicalPosition =
        effects->cursorPos() - cursor.hotSpot();
    const QRect logicalCursorRect =
        QRectF(logicalPosition, cursorSize).toAlignedRect();

    if (logicalCursorRect.isEmpty()) {
        return;
    }

    /*
     * RenderViewport::mapToRenderTarget(Rect) returns a render-target Rect
     * in KWin 6.7.5. Convert it to a Region before intersecting it with
     * deviceRegion; Rect & Region is not a supported operation.
     *
     * mapToRenderTarget() also applies the viewport's scale, render offset,
     * and output transform, so this is deliberately not a hand-written
     * "position * scale" calculation.
     */
    const Region cursorRenderRegion =
        Region(viewport.mapToRenderTarget(KWin::Rect(logicalCursorRect.x(),
                                                       logicalCursorRect.y(),
                                                       logicalCursorRect.width(),
                                                       logicalCursorRect.height()))) & deviceRegion;

    if (cursorRenderRegion.isEmpty()) {
        return;
    }

    /*
     * projectionMatrix() contains the output transform. The model
     * translation therefore uses the viewport's device coordinates before
     * that transform is applied.
     */
    const QPointF devicePosition =
        viewport.mapToDeviceCoordinates(QRectF(logicalPosition, cursorSize)).topLeft();
    const QSizeF deviceSize = cursorSize * viewport.scale();

    GLboolean previousLogicOpEnabled = GL_FALSE;
    GLboolean previousColorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLint previousLogicOp = GL_COPY;

    glGetBooleanv(GL_COLOR_LOGIC_OP, &previousLogicOpEnabled);
    glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);
    glGetIntegerv(GL_LOGIC_OP_MODE, &previousLogicOp);

    glEnable(GL_COLOR_LOGIC_OP);
    glLogicOp(GL_XOR);

    /*
     * XORing alpha would modify the destination alpha channel as well.
     * Cursor inversion is an RGB operation, so preserve the destination
     * alpha channel.
     */
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    auto shader = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(devicePosition.x(), devicePosition.y());
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    cursorTexture->render(cursorRenderRegion, deviceSize, true);

    ShaderManager::instance()->popShader();

    glColorMask(previousColorMask[0],
                previousColorMask[1],
                previousColorMask[2],
                previousColorMask[3]);
    glLogicOp(previousLogicOp);

    if (!previousLogicOpEnabled) {
        glDisable(GL_COLOR_LOGIC_OP);
    }
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (pos != old) {
        updateCursorGeometry();
    }
}

} // namespace KWin

#include "moc_xorcursor.cpp"
