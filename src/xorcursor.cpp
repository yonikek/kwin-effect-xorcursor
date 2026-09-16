/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glshadermanager.h"
#include "opengl/glutils.h"

namespace KWin
{

namespace
{

const QByteArray s_subtractiveCursorFragmentShader = QByteArrayLiteral(R"GLSL(
vec4 tex = texture(sampler, texcoord0);

// Transparent cursor texels must not participate in subtractive blending:
// otherwise src(0) - dst clamps to black and paints the whole cursor rectangle.
if (tex.a <= 0.0) {
    discard;
}

// The blend stage turns this into: 1 - destination.
fragColor = vec4(1.0);
)GLSL");

} // namespace

XorCursorEffect::XorCursorEffect()
{
    m_subtractiveShader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        QByteArray(),
        s_subtractiveCursorFragmentShader);
    if (!m_subtractiveShader) {
        return;
    }
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
        effects->showCursor();
        m_cursorTexture.reset();
        m_isMouseHidden = false;
    }
}

void XorCursorEffect::hideCursor()
{
    if (!m_isMouseHidden && m_subtractiveShader) {
        GLTexture *texture = nullptr;
        if (effects->isOpenGLCompositing()) {
            texture = ensureCursorTexture();
        }
        if (texture) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
        }
    }
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden || !m_subtractiveShader) {
        return;
    }

    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return;
    }

    const auto cursor = effects->cursorImage();
    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();

    // Store the exact logical bounding box for the next frame's cleanup.
    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

    const auto scale = viewport.scale();
    const QRectF cursorDeviceRect(
        p.x() * scale,
        p.y() * scale,
        cursorSize.width() * scale,
        cursorSize.height() * scale);
    const Region cursorRegion = Region(Rect(cursorDeviceRect.toAlignedRect()));

    // Keep the original repaint-optimisation path: render only the cursor's
    // region, then draw the cursor once more using the subtractive operation.
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);

    ShaderManager::instance()->pushShader(m_subtractiveShader.get());
    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    m_subtractiveShader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    m_subtractiveShader->setUniform(GLShader::IntUniform::Sampler, 0);
    cursorTexture->render(cursorSize * scale);
    ShaderManager::instance()->popShader();

    // Restore KWin's usual blend state before leaving the effect.
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (pos != old) {
        const auto cursor = effects->cursorImage();
        const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();

        const QRect newRect = QRectF(pos - cursor.hotSpot(), cursorSize).toAlignedRect();
        effects->addRepaint(KWin::Rect(m_lastCursorRect));
        effects->addRepaint(KWin::Rect(newRect));
    }
}

} // namespace KWin
#include "moc_xorcursor.cpp"
