/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "opengl/gltexture.h"

#include <QImage>

namespace KWin {

    // ---------------------------------------------------------------------------
    // XOR fragment shader (GLSL 1.10, compatible with KWin's shader pipeline).
    //
    // Inspired by the invert effect's minimal shader, this version uses a single
    // packed lookup texture to perform bitwise XOR on all three colour channels
    // in one texture fetch.  The cursor's alpha channel is preserved so that any
    // cursor theme works (no white cursor required).
    // ---------------------------------------------------------------------------
    static const char kXorShaderSource[] = R"(
uniform sampler2D cursorTexture;
uniform sampler2D backgroundTexture;
uniform sampler2D xorLookup;
uniform vec2 cursorSize;

void main()
{
    vec2 uv = gl_TexCoord[0].xy;
    vec4 cursor = texture2D(cursorTexture, uv);
    vec4 background = texture2D(backgroundTexture, uv);

    // Convert 0..1 colour components to 0..255 integers for the lookup.
    // Using floor() avoids the extra +0.5 bias; GL_NEAREST filtering on the
    // lookup texture handles rounding to the nearest texel.
    float cr = floor(cursor.r * 255.0);
    float cg = floor(cursor.g * 255.0);
    float cb = floor(cursor.b * 255.0);
    float br = floor(background.r * 255.0);
    float bg = floor(background.g * 255.0);
    float bb = floor(background.b * 255.0);

    // Packed lookup: the texture stores (r^x, g^y, b^z, 255) for every
    // (cursor channel, background channel) pair.  A single sample returns
    // the XOR result for all three channels simultaneously.
    vec4 xorResult = texture2D(xorLookup, vec2(
        (cr + br * 256.0 + 0.5) / 65536.0,
                                               0.5));

    // Preserve the cursor's alpha so transparent parts leave the background
    // untouched.
    gl_FragColor = vec4(xorResult.rgb, cursor.a);
}
)";

XorCursorEffect::XorCursorEffect()
{
    hideCursor();
    createXorLookupTexture();
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
        disconnect(effects, &EffectsHandler::cursorShapeChanged,
                   this, &XorCursorEffect::markCursorTextureDirty);
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
            connect(effects, &EffectsHandler::cursorShapeChanged,
                    this, &XorCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged,
                    this, &XorCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
        }
    }
}

void XorCursorEffect::createXorLookupTexture()
{
    // Single 256×256 RGBA texture storing the XOR of all three channels.
    // Index = cursor_byte + background_byte * 256, laid out as a 1‑D
    // sequence of texels (width = 65536, height = 1).
    constexpr int kLookupSize = 65536;
    QImage lookup(kLookupSize, 1, QImage::Format_RGBA8888);

    for (int i = 0; i < kLookupSize; ++i) {
        const uchar a = static_cast<uchar>(i & 0xFF);
        const uchar b = static_cast<uchar>((i >> 8) & 0xFF);
        lookup.setPixelColor(i, 0, QColor(a ^ b, a ^ b, a ^ b, 255));
    }

    m_xorLookupTexture = GLTexture::upload(lookup);
    if (m_xorLookupTexture) {
        m_xorLookupTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_xorLookupTexture->setFilter(GL_NEAREST);
    }
}

void XorCursorEffect::ensureBackgroundTexture(const QSize &deviceSize)
{
    if (m_backgroundTexture && m_backgroundTextureSize == deviceSize) {
        return;
    }
    m_backgroundTexture.reset();
    m_backgroundTextureSize = deviceSize;

    QImage dummy(deviceSize, QImage::Format_RGBA8888);
    dummy.fill(Qt::transparent);
    m_backgroundTexture = GLTexture::upload(dummy);
    if (m_backgroundTexture) {
        m_backgroundTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_backgroundTexture->setFilter(GL_NEAREST);
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
        return;
    }

    const auto cursor = effects->cursorImage();
    QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();

    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

    const auto scale = viewport.scale();
    QRectF cursorDeviceRect(p.x() * scale, p.y() * scale,
                            cursorSize.width() * scale,
                            cursorSize.height() * scale);
    Region cursorRegion = Region(Rect(cursorDeviceRect.toAlignedRect()));

    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    const QSize deviceSize = cursorDeviceRect.toAlignedRect().size();
    ensureBackgroundTexture(deviceSize);

    if (m_backgroundTexture) {
        m_backgroundTexture->bind();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            cursorDeviceRect.x(), cursorDeviceRect.y(),
                            deviceSize.width(), deviceSize.height());
    }

    auto shader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture | ShaderTrait::TransformColorspace,
        QByteArray(),
                                                                  QByteArray(kXorShaderSource));

    if (shader) {
        ShaderManager::instance()->pushShader(shader.get());
        shader->setUniform("cursorTexture", 0);
        shader->setUniform("backgroundTexture", 1);
        shader->setUniform("xorLookup", 2);
        shader->setUniform("cursorSize", QVector2D(cursorSize.width() * scale,
                                                   cursorSize.height() * scale));

        glActiveTexture(GL_TEXTURE0);
        cursorTexture->bind();
        glActiveTexture(GL_TEXTURE1);
        if (m_backgroundTexture) {
            m_backgroundTexture->bind();
        }
        glActiveTexture(GL_TEXTURE2);
        if (m_xorLookupTexture) {
            m_xorLookupTexture->bind();
        }

        QMatrix4x4 mvp = viewport.projectionMatrix();
        mvp.translate(p.x() * scale, p.y() * scale);
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
        cursorTexture->render(cursorSize * scale);

        ShaderManager::instance()->popShader();

        glActiveTexture(GL_TEXTURE0);
    }

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
        QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();

        QRect newRect = QRectF(pos - cursor.hotSpot(), cursorSize).toAlignedRect();

        effects->addRepaint(KWin::Rect(m_lastCursorRect));
        effects->addRepaint(KWin::Rect(newRect));
    }
}

} // namespace KWin
#include "moc_xorcursor.cpp"
