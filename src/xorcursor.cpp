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

namespace KWin {

    // ---------------------------------------------------------------------------
    // Returns the major/minor GLSL version from the driver, e.g. "1.40" -> (1,40).
    // Falls back to (1, 10) if the string cannot be parsed.
    // ---------------------------------------------------------------------------
    static Version parseGlslVersion()
    {
        const auto *versionString = glGetString(GL_SHADING_LANGUAGE_VERSION);
        if (!versionString) {
            return Version(1, 10);
        }

        const QByteArray str = QByteArray::fromRawData(
            reinterpret_cast<const char *>(versionString),
                                                       qstrlen(reinterpret_cast<const char *>(versionString)));

        // The string can look like "1.40", "4.60 NVIDIA", "3.00 ES", "OpenGL ES GLSL ES 3.00", etc.
        // Find the first x.y pattern.
        const QRegularExpression re(QStringLiteral(R"((\d+)\.(\d+))"));
    const auto match = re.match(QString::fromLatin1(str));
    if (!match.hasMatch()) {
        return Version(1, 10);
    }

    bool ok = false;
    const int major = match.captured(1).toInt(&ok);
    if (!ok) {
        return Version(1, 10);
    }
    const int minor = match.captured(2).toInt(&ok);
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
// XOR fragment shaders.
//
// `generateCustomShader` replaces the generated fragment shader entirely, so
// these sources must be self-contained. The generated vertex shader for
// ShaderTrait::MapTexture declares:
//
//     attribute/in  vec4 position;
//     attribute/in  vec4 texcoord;
//     varying/out   vec2 texcoord0;
//
// and writes `texcoord0 = texcoord.st;`. It does NOT use gl_TexCoord, so we
// must sample with `texcoord0`.
//
// The fragment shader must declare `uniform sampler2D sampler;` and the
// `texcoord0` varying itself, because the generated fragment header is not
// included when a custom fragment source is supplied.
// ---------------------------------------------------------------------------

// GLSL 1.10 / ES 1.00 variant (used when the context GLSL version < 1.40)
static const char kXorShaderSourceLegacy[] = R"(
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
uniform sampler2D xorLookup;
varying vec2 texcoord0;

void main()
{
    vec4 cursor     = texture2D(sampler,         texcoord0);
    vec4 background = texture2D(backgroundTexture, texcoord0);

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

// GLSL 1.40+ / ES 3.00 variant.
// KWin's GLShader::prepareSource rewrites "#version 140" to
// "#version 300 es\n\nprecision highp float;\n" for ES 3.00 contexts, so this
// single source works for both desktop GLSL 1.40+ and GLSL ES 3.00.
static const char kXorShaderSourceModern[] = R"(
#version 140
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
uniform sampler2D xorLookup;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 cursor     = texture(sampler,         texcoord0);
    vec4 background = texture(backgroundTexture, texcoord0);

    float cr = floor(cursor.r     * 255.0 + 0.5);
    float cg = floor(cursor.g     * 255.0 + 0.5);
    float cb = floor(cursor.b     * 255.0 + 0.5);
    float br = floor(background.r * 255.0 + 0.5);
    float bg = floor(background.g * 255.0 + 0.5);
    float bb = floor(background.b * 255.0 + 0.5);

    float xr = texture(xorLookup, vec2((cr + 0.5) / 256.0, (br + 0.5) / 256.0)).r;
    float xg = texture(xorLookup, vec2((cg + 0.5) / 256.0, (bg + 0.5) / 256.0)).r;
    float xb = texture(xorLookup, vec2((cb + 0.5) / 256.0, (bb + 0.5) / 256.0)).r;

    fragColor = vec4(xr, xg, xb, cursor.a);
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
        qCWarning(KWINEFFECTS) << "XorCursorEffect: failed to create XOR lookup texture";
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

    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    ensureBackgroundTexture(deviceSize);
    if (m_backgroundTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_backgroundTexture->bind();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            cursorDeviceRect.x(), cursorDeviceRect.y(),
                            deviceSize.width(), deviceSize.height());
        glActiveTexture(GL_TEXTURE0);
    }

    const QByteArray fragmentSource = useModernGlsl()
    ? QByteArray(kXorShaderSourceModern)
    : QByteArray(kXorShaderSourceLegacy);

    auto shader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        QByteArray(),
                                                                  fragmentSource);

    if (!shader) {
        qCWarning(KWINEFFECTS) << "XorCursorEffect: custom shader unavailable";
        return;
    }

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldBlendSrc = 0, oldBlendDst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldBlendSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldBlendDst);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ShaderManager::instance()->pushShader(shader.get());
    shader->setUniform("sampler", 0);
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

    // Draw a textured quad using KWin's modern GLVertexBuffer API.
    const QSizeF size = cursorSize * scale;
    const QRectF rect(0.0, 0.0, size.width(), size.height());

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();

    const std::array<GLVertex2D, 4> vertices = {{
        {QVector2D(rect.left(),  rect.top()),    QVector2D(0.0f, 1.0f)},
        {QVector2D(rect.right(), rect.top()),    QVector2D(1.0f, 1.0f)},
        {QVector2D(rect.left(),  rect.bottom()), QVector2D(0.0f, 0.0f)},
        {QVector2D(rect.right(), rect.bottom()), QVector2D(1.0f, 0.0f)},
    }};

    vbo->setVertices(vertices);
    vbo->render(GL_TRIANGLE_STRIP);

    ShaderManager::instance()->popShader();

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
