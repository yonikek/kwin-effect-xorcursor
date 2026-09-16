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

#include <cmath>

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

    // Keep this in sync with KWin's accessibility InvertEffect: perform the
    // inversion in gamma 2.2 encoding space and convert back afterwards.
    vec4 encoded = nitsToEncoding(scene, gamma22_EOTF, 0.0, destinationReferenceLuminance);
    encoded.rgb /= max(0.001, encoded.a);
    encoded.rgb = vec3(1.0) - encoded.rgb;
    encoded.rgb *= encoded.a;
    encoded = encodingToNits(encoded, gamma22_EOTF, 0.0, destinationReferenceLuminance);

    vec4 normal = nitsToDestinationEncoding(scene);
    vec4 inverted = nitsToDestinationEncoding(encoded);

    // The cursor image is uploaded in Qt's top-left-origin convention, while
    // KWin's GLTexture render path flips Y for OpenGL. Sample the cursor mask
    // with the inverse Y so the mask lines up with the rendered cursor.
    float mask = texture(cursorSampler, vec2(texcoord0.x, 1.0 - texcoord0.y)).a;
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

    // Keep the allocation alive when the cursor moves or when a cursor shape
    // becomes smaller. Reallocate only when we actually need more storage.
    if (m_backgroundTexture && m_backgroundFramebuffer
        && m_backgroundTexture->size().width() >= size.width()
        && m_backgroundTexture->size().height() >= size.height()) {
        return true;
    }

    m_backgroundFramebuffer.reset();
    m_backgroundTexture.reset();

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

    // The logical rectangle is used only for mapping the source region through
    // RenderViewport. The device rectangle deliberately gets a size which is
    // independent of the cursor position, so moving over fractional pixels
    // cannot cause the scratch texture to be resized every frame.
    const QRect cursorLogicalRect = QRectF(p, cursorSize).toAlignedRect();
    const QSize cursorDeviceSize(
        std::max(1, int(std::ceil(cursorSize.width() * scale))),
        std::max(1, int(std::ceil(cursorSize.height() * scale))));
    const QPoint cursorDevicePos(
        int(std::floor(p.x() * scale)),
        int(std::floor(p.y() * scale)));
    const QRect cursorRect(cursorDevicePos, cursorDeviceSize);
    const Region cursorRegion{Rect(cursorRect)};

    // Paint the cursor's underlying region first, then replace it with the
    // color-managed inverted version. This keeps the repaint optimization
    // while avoiding GL_COLOR_LOGIC_OP / bitwise framebuffer XOR.
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);
    if (!ensureBackgroundBuffer(cursorRect.size())) {
        return;
    }

    // Prefer a direct texture copy for the common untransformed framebuffer
    // case. This avoids the extra destination-FBO blit involved in
    // GLFramebuffer::blitFromRenderTarget(). Fall back to KWin's blit helper
    // for transformed targets and non-FBO render targets.
    bool copied = false;
    const Rect sourceRect = viewport.mapToRenderTarget(Rect(cursorLogicalRect));
    GLFramebuffer *currentFramebuffer = GLFramebuffer::currentFramebuffer();
    if (renderTarget.framebuffer() == currentFramebuffer
        && viewport.transform() == OutputTransform::Normal
        && renderTarget.size() == viewport.deviceSize()
        && sourceRect.size() == cursorRect.size()
        && sourceRect.x() >= 0
        && sourceRect.y() >= 0
        && sourceRect.right() <= renderTarget.size().width() - 1
        && sourceRect.bottom() <= renderTarget.size().height() - 1) {
        glActiveTexture(GL_TEXTURE0);
        m_backgroundTexture->bind();

        const int sourceY = renderTarget.size().height() - (sourceRect.y() + sourceRect.height());
        glCopyTexSubImage2D(GL_TEXTURE_2D,
                            0,
                            0,
                            0,
                            sourceRect.x(),
                            sourceY,
                            sourceRect.width(),
                            sourceRect.height());

        m_backgroundTexture->unbind();
        copied = true;
    }

    if (!copied) {
        // Destination is always the top-left portion of the persistent scratch
        // texture. The texture can be larger than the current cursor and the
        // shader samples only this source rectangle below.
        if (!m_backgroundFramebuffer->blitFromRenderTarget(renderTarget,
                                                            viewport,
                                                            cursorLogicalRect,
                                                            Rect(QPoint(), cursorRect.size()))) {
            return;
        }
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

    glActiveTexture(GL_TEXTURE0);
    // When the persistent scratch texture is larger than the current cursor,
    // render only the portion containing this frame's copied background.
    const QRectF source(0, 0, cursorRect.width(), cursorRect.height());
    m_backgroundTexture->render(source, Region::infinite(), cursorRect.size());

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
