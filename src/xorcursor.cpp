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

    vec4 inverted = nitsToDestinationEncoding(encoded);

    // The cursor image is uploaded in Qt's top-left-origin convention, while
    // KWin's GLTexture render path flips Y for OpenGL. Sample the cursor mask
    // with the inverse Y so the mask lines up with the rendered cursor.
    float mask = texture(cursorSampler, vec2(texcoord0.x, 1.0 - texcoord0.y)).a;

    // The scratch target covers a rectangle, but only the cursor shape should
    // affect the real render target. Discard the transparent part of the cursor
    // so the scratch rectangle itself can never overwrite the screen. For the
    // actual cursor pixels, preserve the rendered scene and interpolate toward
    // the inverted version using the cursor alpha. This also avoids depending
    // on the caller having a particular blend state.
    if (mask <= 0.001) {
        discard;
    }
    fragColor = mix(scene, inverted, mask);
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
    if (!ensureBackgroundBuffer(cursorRect.size())) {
        return;
    }

    // Render the same small logical region directly into the persistent scratch
    // framebuffer. This is the key performance change: it avoids copying pixels
    // out of the live render target, which can introduce a GPU synchronization
    // point on some drivers.
    //
    // The scratch viewport makes the cursor rectangle the entire render target,
    // while preserving the output scale. The scratch target uses the same color
    // description as the real render target, so the inversion shader can use the
    // same color-management uniforms as KWin's accessibility InvertEffect.
    RenderTarget scratchTarget(m_backgroundFramebuffer.get(), renderTarget.colorDescription());
    RenderViewport scratchViewport(RectF(cursorLogicalRect), scale, scratchTarget, QPoint());
    const Region scratchRegion{Rect(QPoint(), cursorRect.size())};

    GLFramebuffer::pushFramebuffer(m_backgroundFramebuffer.get());
    effects->paintScreen(scratchTarget, scratchViewport, mask, scratchRegion, screen);
    GLFramebuffer::popFramebuffer();

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
