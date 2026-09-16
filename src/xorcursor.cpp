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
    // XOR fragment shader (GLSL 1.10, using KWin's MapTexture conventions).
    //
    //   * `u_texture` and `varying vec2 texcoord` are provided automatically by
    //     ShaderTrait::MapTexture, so we must NOT redeclare them.
    //   * `backgroundTexture` holds a snapshot of the framebuffer region behind
    //     the cursor, captured with glCopyTexSubImage2D right before drawing.
    //   * `xorLookup` is a 256x256 RGBA texture where texel (x, y) stores
    //     (x ^ y) in its red channel.  GLSL 1.10 has no bitwise operators, so
    //     this lookup is the only portable way to perform exact bitwise XOR.
    //   * The cursor's alpha channel is preserved so any cursor theme works.
    // ---------------------------------------------------------------------------
    static const char kXorShaderSource[] = R"(
uniform sampler2D backgroundTexture;
uniform sampler2D xorLookup;

void main()
{
    vec4 cursor     = texture2D(u_texture,         texcoord);
    vec4 background = texture2D(backgroundTexture, texcoord);

    float cr = floor(cursor.r     * 255.0 + 0.5);
    float cg = floor(cursor.g     * 255.0 + 0.5);
    float cb = floor(cursor.b     * 255.0 + 0.5);
    float br = floor(background.r * 255.0 + 0.5);
    float bg = floor(background.g * 255.0 + 0.5);
    float bb = floor(background.b * 255.0 + 0.5);

    float xr = texture2D(xorLookup, vec2((cr + 0.5) / 256.0, (br + 0.5) / 256.0)).r;
    float xg = texture2D(xorLookup, vec2((cg + 0.5) / 256.0, (bg + 0.5) / 256.0)).r;
    float xb = texture2D(xorLookup, vec2((cb + 0.5) / 256.0, (bb + 0.5) / 256.0)).r;

    gl_FragColor = vec4(xr, xg, xb, cursor.a);
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
    // 256x256 RGBA texture: texel (x, y) = x ^ y in the red channel.
    QImage lookup(256, 256, QImage::Format_RGBA8888);
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            const uchar x = static_cast<uchar>(a ^ b);
            lookup.setPixelColor(a, b, QColor(x, x, x, 255));
        }
    }

    m_xorLookupTexture = GLTexture::upload(lookup);
    if (m_xorLookupTexture) {
        m_xorLookupTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_xorLookupTexture->setFilter(GL_NEAREST);
    } else {
        qWarning(KWIN_EFFECT_LOG) << "XorCursorEffect: failed to create XOR lookup texture";
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
    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();

    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

    const auto scale = viewport.scale();
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale,
                                  cursorSize.width() * scale,
                                  cursorSize.height() * scale);
    const QSize deviceSize = cursorDeviceRect.toAlignedRect().size();
    const Region cursorRegion(Rect(cursorDeviceRect.toAlignedRect()));

    // Re-paint just the cursor region so any effects that draw there are
    // included in the background we are about to XOR against.
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    // -----------------------------------------------------------------------
    // 1. Capture the background behind the cursor into m_backgroundTexture.
    // -----------------------------------------------------------------------
    ensureBackgroundTexture(deviceSize);
    if (m_backgroundTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_backgroundTexture->bind();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            cursorDeviceRect.x(), cursorDeviceRect.y(),
                            deviceSize.width(), deviceSize.height());
        glActiveTexture(GL_TEXTURE0);
    }

    // -----------------------------------------------------------------------
    // 2. Draw the cursor using our custom XOR shader.
    // -----------------------------------------------------------------------
    auto shader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        QByteArray(),
                                                                  QByteArray(kXorShaderSource));

    if (!shader) {
        qWarning(KWIN_EFFECT_LOG) << "XorCursorEffect: custom shader unavailable, "
        "falling back to plain cursor rendering";
        cursorTexture->render(cursorSize * scale);
        return;
    }

    // Save blend state so we don't leak it to the rest of the compositor.
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldBlendSrc = 0, oldBlendDst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDst);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ShaderManager::instance()->pushShader(shader.get());
    shader->setUniform("u_texture", 0);
    shader->setUniform("backgroundTexture", 1);
    shader->setUniform("xorLookup", 2);

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
    glActiveTexture(GL_TEXTURE0);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    // Use the low-level textured-quad draw rather than GLTexture::render(),
    // because the latter can push its own shader and shadow ours.
    {
        const QSizeF size = cursorSize * scale;
        const QRectF rect(0.0, 0.0, size.width(), size.height());
        GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
        vbo->reset();
        vbo->setUseColorSpace(false);
        vbo->setAttribLayout(GLVertexBuffer::GLVertex2DLayout,
                             sizeof(GLVertex2D), 4);
        GLVertex2D *map = static_cast<GLVertex2D *>(vbo->map(sizeof(GLVertex2D) * 4));
        const QRectF &r = rect;
        map[0] = GLVertex2D{r.x(),         r.y(),         0.0f, 1.0f};
        map[1] = GLVertex2D{r.right(),     r.y(),         1.0f, 1.0f};
        map[2] = GLVertex2D{r.x(),         r.bottom(),    0.0f, 0.0f};
        map[3] = GLVertex2D{r.right(),     r.bottom(),    1.0f, 0.0f};
        vbo->unmap();
        vbo->bindArrays();
        vbo->draw(GL_TRIANGLE_STRIP, 0, 4);
        vbo->unbindArrays();
    }

    ShaderManager::instance()->popShader();

    // Restore blend state.
    glBlendFunc(oldBlendSrc, oldBlendDst);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
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
