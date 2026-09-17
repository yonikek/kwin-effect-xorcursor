/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/glvertexbuffer.h"

#include <QImage>
#include <QOpenGLContext>
#include <QRegularExpression>

#include <array>

namespace KWin {

    // ---------------------------------------------------------------------------
    // GLSL version detection
    //
    // We need a fragment shader whose "#version" matches the one KWin injects
    // into the generated vertex shader. KWin uses "#version 140" for GLSL 1.40+
    // contexts and no version directive otherwise. Query the driver directly
    // because GLPlatform is not exported to plugins.
    // ---------------------------------------------------------------------------
    static Version parseGlslVersion()
    {
        const auto *s = glGetString(GL_SHADING_LANGUAGE_VERSION);
        if (!s) {
            return Version(1, 10);
        }

        const QByteArray str = QByteArray::fromRawData(
            reinterpret_cast<const char *>(s),
                                                       qstrlen(reinterpret_cast<const char *>(s)));

        // Match strings like "1.40", "4.60 NVIDIA", "OpenGL ES GLSL ES 3.00".
        const QRegularExpression re(QStringLiteral(R"((\d+)\.(\d+))"));
    const auto m = re.match(QString::fromLatin1(str));
    if (!m.hasMatch()) {
        return Version(1, 10);
    }

    bool ok = false;
    const int major = m.captured(1).toInt(&ok);
    if (!ok) {
        return Version(1, 10);
    }
    const int minor = m.captured(2).toInt(&ok);
    if (!ok) {
        return Version(1, 10);
    }

    return Version(major, minor);
    }

static bool useModernGlsl()
{
    return parseGlslVersion() >= Version(1, 40);
}

// ---------------------------------------------------------------------------
// Fragment shaders.
//
// The generated vertex shader for ShaderTrait::MapTexture writes a
// `varying vec2 texcoord0`, NOT `gl_TexCoord[0]`. The fragment shader
// generated header is skipped when a custom fragment source is supplied,
// so we must declare `sampler` and `texcoord0` ourselves.
//
// `1.0 - x` on a normalized 8-bit colour component is bit-exact
// `x XOR 0xFF`, which is the classic bitwise NOT the X11 XorCursor applied
// wherever its 1-bit mask was set.
// ---------------------------------------------------------------------------
static const char kShaderLegacy[] = R"(
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
varying vec2 texcoord0;

void main()
{
    vec4 cursor = texture2D(sampler, texcoord0);
    vec4 bg = texture2D(backgroundTexture, texcoord0);
    vec3 inverted = vec3(1.0) - bg.rgb;
    gl_FragColor = vec4(inverted, cursor.a);
}
)";

// KWin rewrites "#version 140" to "#version 300 es\nprecision highp float;"
// on GLSL ES 3.00 contexts, so this single source covers both.
static const char kShaderModern[] = R"(
#version 140
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 cursor = texture(sampler, texcoord0);
    vec4 bg = texture(backgroundTexture, texcoord0);
    vec3 inverted = vec3(1.0) - bg.rgb;
    fragColor = vec4(inverted, cursor.a);
}
)";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

XorCursorEffect::XorCursorEffect()
{
    connect(effects, &EffectsHandler::cursorShapeChanged,
            this, &XorCursorEffect::slotCursorShapeChanged);
    connect(effects, &EffectsHandler::mouseChanged,
            this, &XorCursorEffect::slotMouseChanged);
    tryAcquireHide();
}

XorCursorEffect::~XorCursorEffect()
{
    disconnect(effects, &EffectsHandler::cursorShapeChanged,
               this, &XorCursorEffect::slotCursorShapeChanged);
    disconnect(effects, &EffectsHandler::mouseChanged,
               this, &XorCursorEffect::slotMouseChanged);
    releaseHide();
}

// ---------------------------------------------------------------------------
// Hide-state management
// ---------------------------------------------------------------------------

void XorCursorEffect::tryAcquireHide()
{
    if (m_hideAcquired) {
        return;
    }
    if (!effects->isOpenGLCompositing()) {
        return;
    }
    // The cursor image may not be ready at construction time. If it isn't,
    // we'll retry from slotCursorShapeChanged().
    if (!cursorTexture()) {
        return;
    }
    effects->hideCursor();
    m_hideAcquired = true;
}

void XorCursorEffect::releaseHide()
{
    if (!m_hideAcquired) {
        return;
    }
    effects->showCursor();
    m_cursorTexture.reset();
    m_hideAcquired = false;
}

bool XorCursorEffect::isHiddenByOtherEffect()
{
    // The platform maintains a reference count of hide requests. To find
    // out whether anyone besides us is holding the cursor hidden, briefly
    // drop our own request and read the counter. Because the platform
    // only performs the actual show/hide transitions when the count
    // crosses zero, and this runs before any frame is committed, the
    // toggle is invisible.
    effects->showCursor();
    const bool hiddenByOther = effects->isCursorHidden();
    effects->hideCursor();
    return hiddenByOther;
}

// ---------------------------------------------------------------------------
// Cursor texture
// ---------------------------------------------------------------------------

GLTexture *XorCursorEffect::cursorTexture()
{
    if (!m_cursorTexture || m_cursorTextureDirty) {
        m_cursorTexture.reset();
        m_cursorTextureDirty = false;
        const auto cursor = effects->cursorImage();
        if (!cursor.image().isNull()) {
            m_cursorTexture = GLTexture::upload(cursor.image());
            if (m_cursorTexture) {
                m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
                m_cursorTexture->setFilter(GL_NEAREST);
            }
        }
    }
    return m_cursorTexture.get();
}

void XorCursorEffect::invalidateCursorTexture()
{
    m_cursorTextureDirty = true;
}

// ---------------------------------------------------------------------------
// Background capture
// ---------------------------------------------------------------------------

void XorCursorEffect::ensureBackgroundTexture(const QSize &size)
{
    if (m_backgroundTexture && m_backgroundTextureSize == size) {
        return;
    }
    m_backgroundTexture.reset();
    m_backgroundTextureSize = size;

    QImage dummy(size, QImage::Format_RGBA8888);
    dummy.fill(Qt::transparent);
    m_backgroundTexture = GLTexture::upload(dummy);
    if (m_backgroundTexture) {
        m_backgroundTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_backgroundTexture->setFilter(GL_NEAREST);
    }
}

// ---------------------------------------------------------------------------
// Geometry helper
// ---------------------------------------------------------------------------

QRect XorCursorEffect::cursorLogicalRect() const
{
    const auto cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        return {};
    }
    const QSizeF size = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF pos = effects->cursorPos() - cursor.hotSpot();
    return QRectF(pos, size).toAlignedRect();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget,
                                  const RenderViewport &viewport,
                                  int mask,
                                  const Region &deviceRegion,
                                  LogicalOutput *screen)
{
    // 1. Render everything below this effect in the damage region.
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (!m_hideAcquired) {
        return;
    }

    // 2. Defensive: if some other effect has unbalanced its show/hide calls
    //    and left the cursor visible, restore our hide before drawing.
    if (!effects->isCursorHidden()) {
        effects->hideCursor();
    }

    // 3. If another effect is also hiding the cursor (shake, zoom, etc.),
    //    do not draw ours: we would conflict with whatever it renders.
    if (isHiddenByOtherEffect()) {
        return;
    }

    // 4. Fetch (or lazily build) the cursor texture.
    GLTexture *tex = cursorTexture();
    if (!tex) {
        return;
    }

    const auto cursor = effects->cursorImage();
    const QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF pos = effects->cursorPos() - cursor.hotSpot();

    m_lastCursorRect = QRectF(pos, cursorSize).toAlignedRect();

    const auto scale = viewport.scale();
    const QRectF deviceRectF(pos.x() * scale, pos.y() * scale,
                             cursorSize.width() * scale,
                             cursorSize.height() * scale);
    const QRect deviceRect = deviceRectF.toAlignedRect();
    const QSize deviceSize = deviceRect.size();

    // 5. Capture the background behind the cursor. KWin's RenderTarget
    //    framebuffer is top-down relative to the render viewport, so
    //    deviceRect.y() is the correct source Y: no flip.
    ensureBackgroundTexture(deviceSize);
    if (!m_backgroundTexture) {
        return;
    }

    glActiveTexture(GL_TEXTURE1);
    m_backgroundTexture->bind();
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        deviceRect.x(), deviceRect.y(),
                        deviceSize.width(), deviceSize.height());
    glActiveTexture(GL_TEXTURE0);

    // 6. Compile or retrieve the shader (KWin caches by source + traits).
    const QByteArray source = useModernGlsl()
    ? QByteArray(kShaderModern)
    : QByteArray(kShaderLegacy);
    auto shader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture, QByteArray(), source);
    if (!shader) {
        qWarning() << "XorCursorEffect: custom shader unavailable";
        return;
    }

    // 7. Save GL blend state so we don't leak it to the rest of the chain.
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldSrc = 0, oldDst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldDst);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 8. Bind and draw.
    ShaderManager::instance()->pushShader(shader.get());
    shader->setUniform("sampler", 0);
    shader->setUniform("backgroundTexture", 1);

    glActiveTexture(GL_TEXTURE0);
    tex->bind();
    glActiveTexture(GL_TEXTURE1);
    m_backgroundTexture->bind();
    glActiveTexture(GL_TEXTURE0);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(pos.x() * scale, pos.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    const QRectF rect(0.0, 0.0, cursorSize.width() * scale, cursorSize.height() * scale);
    const std::array<GLVertex2D, 4> vertices = {{
        {QVector2D(rect.left(),  rect.top()),    QVector2D(0.0f, 0.0f)},
        {QVector2D(rect.right(), rect.top()),    QVector2D(1.0f, 0.0f)},
        {QVector2D(rect.left(),  rect.bottom()), QVector2D(0.0f, 1.0f)},
        {QVector2D(rect.right(), rect.bottom()), QVector2D(1.0f, 1.0f)},
    }};
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setVertices(vertices);
    vbo->render(GL_TRIANGLE_STRIP);

    ShaderManager::instance()->popShader();

    // 9. Restore blend state.
    glBlendFunc(oldSrc, oldDst);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
}

bool XorCursorEffect::isActive() const
{
    return m_hideAcquired;
}

// ---------------------------------------------------------------------------
// Repaint scheduling
// ---------------------------------------------------------------------------

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (pos == old) {
        return;
    }
    // Repaint only the two rectangles that actually changed: the previous
    // cursor location (to erase the previous inversion) and the new one
    // (to make it ready for the next inversion).
    if (!m_lastCursorRect.isEmpty()) {
        effects->addRepaint(Rect(m_lastCursorRect));
    }
    const QRect newRect = cursorLogicalRect();
    if (!newRect.isEmpty()) {
        effects->addRepaint(Rect(newRect));
    }
}

void XorCursorEffect::slotCursorShapeChanged()
{
    invalidateCursorTexture();
    tryAcquireHide();
    if (!m_hideAcquired) {
        return;
    }
    // The old shape must be erased and the new one drawn. If the shape
    // changed without a move, cursorLogicalRect() returns the new shape's
    // bounding box while m_lastCursorRect holds the old one's.
    if (!m_lastCursorRect.isEmpty()) {
        effects->addRepaint(Rect(m_lastCursorRect));
    }
    const QRect newRect = cursorLogicalRect();
    if (!newRect.isEmpty()) {
        effects->addRepaint(Rect(newRect));
    }
}

} // namespace KWin
#include "moc_xorcursor.cpp"
