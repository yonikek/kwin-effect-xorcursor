/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"

namespace KWin
{

namespace
{

// This is the same color-space-aware inversion used by KWin's built-in
// InvertEffect, with the cursor alpha used as a per-pixel mix mask.
constexpr auto s_invertCursorFragmentShader = R"SHADER(
#version 140
#include "colormanagement.glsl"
#include "saturation.glsl"

uniform sampler2D sampler;
uniform sampler2D cursorSampler;

in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 scene = texture(sampler, texcoord0);
    scene = sourceEncodingToNitsInDestinationColorspace(scene);
    scene = adjustSaturation(scene);

    // This is the same inversion transform used by KWin's accessibility
    // InvertEffect: do the inversion in gamma 2.2 space and convert back to
    // the destination colorspace afterwards.
    vec4 encoded = nitsToEncoding(scene, gamma22_EOTF, 0.0, destinationReferenceLuminance);
    encoded.rgb /= max(0.001, encoded.a);
    encoded.rgb = vec3(1.0) - encoded.rgb;
    encoded.rgb *= encoded.a;
    encoded = encodingToNits(encoded, gamma22_EOTF, 0.0, destinationReferenceLuminance);

    vec4 normal = nitsToDestinationEncoding(scene);
    vec4 inverted = nitsToDestinationEncoding(encoded);
    float mask = texture(cursorSampler, texcoord0).a;
    fragColor = mix(normal, inverted, mask);
}
)SHADER";

}

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
        effects->showCursor();
        m_cursorTexture.reset();
        m_backgroundTexture.reset();
        m_backgroundFramebuffer.reset();
        m_invertShader.reset();
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
        }
    }
}

bool XorCursorEffect::ensureBackgroundBuffer(const QSize &size)
{
    if (size.isEmpty()) {
        return false;
    }
    if (m_backgroundTexture && m_backgroundTexture->size() == size && m_backgroundFramebuffer) {
        return true;
    }

    m_backgroundFramebuffer.reset();
    m_backgroundTexture = GLTexture::allocate(GL_RGBA8, size);
    if (!m_backgroundTexture) {
        return false;
    }
    m_backgroundTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    m_backgroundTexture->setFilter(GL_LINEAR);

    m_backgroundFramebuffer = std::make_unique<GLFramebuffer>(m_backgroundTexture.get());
    return m_backgroundFramebuffer->valid();
}

GLShader *XorCursorEffect::ensureInvertShader()
{
    if (!m_invertShader) {
        m_invertShader = ShaderManager::instance()->generateCustomShader(
            ShaderTrait::MapTexture,
            QByteArray(),
            QByteArray(s_invertCursorFragmentShader));
    }
    return m_invertShader.get();
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

    const auto cursor = effects->cursorImage();
    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

    const qreal scale = viewport.scale();
    const QRect cursorLogicalRect = QRectF(p, cursorSize).toAlignedRect();
    const QRect cursorRect = QRectF(p.x() * scale,
                                    p.y() * scale,
                                    cursorSize.width() * scale,
                                    cursorSize.height() * scale)
                                .toAlignedRect();
    const Region cursorRegion{Rect(cursorRect)};

    // Paint the cursor's underlying region first, then replace it with the
    // color-managed inverted version. This keeps the repaint optimization
    // while avoiding GL_COLOR_LOGIC_OP / bitwise framebuffer XOR.
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);
    if (!ensureBackgroundBuffer(cursorRect.size())) {
        return;
    }

    // Snapshot the already-composited cursor rectangle before sampling it in
    // the inversion shader. Sampling the current framebuffer directly would
    // create a read/write feedback loop.
    if (!m_backgroundFramebuffer->blitFromRenderTarget(renderTarget, viewport, Rect(cursorLogicalRect), Rect(QPoint(), cursorRect.size()))) {
        return;
    }

    GLShader *shader = ensureInvertShader();
    if (!shader) {
        return;
    }

    ShaderBinder binder(shader);
    shader->setUniform(GLShader::IntUniform::Sampler, 0);
    shader->setUniform("cursorSampler", 1);
    shader->setUniform("saturation", 1.0f);
    shader->setColorspaceUniforms(renderTarget.colorDescription(), renderTarget.colorDescription(), RenderingIntent::Perceptual);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(cursorRect.left(), cursorRect.top());
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    glActiveTexture(GL_TEXTURE0);
    m_backgroundTexture->bind();
    glActiveTexture(GL_TEXTURE1);
    cursorTexture->bind();

    // GLTexture::render() binds the texture on the currently active unit, so
    // leave unit 0 active while it draws the snapshot and keep the cursor mask
    // bound on unit 1.
    glActiveTexture(GL_TEXTURE0);
    m_backgroundTexture->render(cursorRect.size());

    glActiveTexture(GL_TEXTURE1);
    cursorTexture->unbind();
    glActiveTexture(GL_TEXTURE0);
    m_backgroundTexture->unbind();
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
