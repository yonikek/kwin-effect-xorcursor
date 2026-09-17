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

#include <KConfigGroup>
#include <KSharedConfig>

namespace KWin {

    // ---------------------------------------------------------------------------
    // Returns the GLSL version parsed from the driver's
    // GL_SHADING_LANGUAGE_VERSION string, e.g. "1.40" -> (1, 40).
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
// XOR cursor fragment shaders.
//
// The classic X11 "XorCursor" behaviour is: the cursor's bitmap acts as a
// 1-bit mask; where the mask is 1, the screen is XORed with 0xFF (i.e.
// bitwise NOT / inversion); where the mask is 0, the screen is untouched.
//
// For normalized 8-bit colour components, "1.0 - x" is exactly
// "x XOR 0xFF", so we get a genuine bitwise XOR with all-ones. This works
// with ANY cursor theme because the cursor's alpha channel is used as the
// mask, not its RGB values.
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
// must sample with `texcoord0`. The fragment shader must declare
// `uniform sampler2D sampler;` and the `texcoord0` varying itself.
// ---------------------------------------------------------------------------

// GLSL 1.10 / ES 1.00 variant (used when the context GLSL version < 1.40)
static const char kInvertShaderSourceLegacy[] = R"(
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
varying vec2 texcoord0;

void main()
{
    vec4 cursor     = texture2D(sampler,           texcoord0);
    vec4 background = texture2D(backgroundTexture, texcoord0);

    // Invert the background: for normalized 8-bit values, 1.0 - x is
    // exactly x XOR 0xFF, the classic bitwise XOR cursor operation.
    vec3 inverted = vec3(1.0) - background.rgb;

    // Use the cursor's alpha channel as the mask. Opaque pixels show the
    // inverted background; transparent pixels leave it untouched.
    gl_FragColor = vec4(inverted, cursor.a);
}
)";

// GLSL 1.40+ / ES 3.00 variant.
// KWin's GLShader::prepareSource rewrites "#version 140" to
// "#version 300 es\n\nprecision highp float;\n" for ES 3.00 contexts, so this
// single source works for both desktop GLSL 1.40+ and GLSL ES 3.00.
static const char kInvertShaderSourceModern[] = R"(
#version 140
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 cursor     = texture(sampler,           texcoord0);
    vec4 background = texture(backgroundTexture, texcoord0);

    vec3 inverted = vec3(1.0) - background.rgb;

    fragColor = vec4(inverted, cursor.a);
}
)";

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

qreal XorCursorEffect::cursorMagnification() const
{
    // Ask KWin whether the shake cursor effect is currently magnifying the
    // cursor. If it is, the XOR cursor must be drawn at the same
    // magnification, otherwise the inverted region would not cover the
    // magnified cursor.
    Effect *shakeEffect = effects->findEffect(QStringLiteral("shakecursor"));
    if (!shakeEffect || !shakeEffect->isActive()) {
        return 1.0;
    }

    // The shake effect reads its magnification from the
    // "Effect-shakecursor" config group. Read the same value so the XOR
    // cursor follows the magnified cursor's target size.
    const KConfigGroup config = effects->config()->group(QStringLiteral("Effect-shakecursor"));
    return config.readEntry("Magnification", 3.0);
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
    // Re-hide the cursor on every frame. If the shake cursor effect's
    // deflation animation finished between frames, it may have called
    // effects->showCursor(), which would make the system cursor visible
    // again even though we still want it hidden.
    if (m_isMouseHidden) {
        effects->hideCursor();
    }

    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (!m_isMouseHidden) {
        return;
    }

    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return;
    }

    const auto cursor = effects->cursorImage();
    const qreal magnification = cursorMagnification();
    m_lastMagnification = magnification;

    // The shake cursor effect scales the cursor around its hotspot: the
    // hotspot stays anchored at the pointer position while the rest of the
    // cursor expands around it. We reproduce that by scaling both the size
    // and the hotspot offset.
    const QSizeF baseSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QSizeF cursorSize = baseSize * magnification;
    const QPointF hotspot = cursor.hotSpot() * magnification;
    const QPointF p = effects->cursorPos() - hotspot;

    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();

    const auto scale = viewport.scale();
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale,
                                  cursorSize.width() * scale,
                                  cursorSize.height() * scale);

    const QRect deviceRect = cursorDeviceRect.toAlignedRect();
    const QSize deviceSize = deviceRect.size();
    const Region cursorRegion = Region(Rect(deviceRect));

    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    ensureBackgroundTexture(deviceSize);
    if (m_backgroundTexture) {
        glActiveTexture(GL_TEXTURE1);
        m_backgroundTexture->bind();
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                            deviceRect.x(), deviceRect.y(),
                            deviceSize.width(), deviceSize.height());
        glActiveTexture(GL_TEXTURE0);
    }

    const QByteArray fragmentSource = useModernGlsl()
    ? QByteArray(kInvertShaderSourceModern)
    : QByteArray(kInvertShaderSourceLegacy);

    auto shader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        QByteArray(),
                                                                  fragmentSource);

    if (!shader) {
        qWarning() << "XorCursorEffect: custom shader unavailable";
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

    glActiveTexture(GL_TEXTURE0);
    cursorTexture->bind();
    glActiveTexture(GL_TEXTURE1);
    if (m_backgroundTexture) {
        m_backgroundTexture->bind();
    }
    glActiveTexture(GL_TEXTURE0);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    // Draw a textured quad. The cursor texture is sampled with (0,0) at the
    // top-left of the image, matching KWin's GLTexture convention. The quad
    // is larger when the shake cursor effect is magnifying.
    const QSizeF size = cursorSize * scale;
    const QRectF rect(0.0, 0.0, size.width(), size.height());

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();

    const std::array<GLVertex2D, 4> vertices = {{
        {QVector2D(rect.left(),  rect.top()),    QVector2D(0.0f, 0.0f)},
        {QVector2D(rect.right(), rect.top()),    QVector2D(1.0f, 0.0f)},
        {QVector2D(rect.left(),  rect.bottom()), QVector2D(0.0f, 1.0f)},
        {QVector2D(rect.right(), rect.bottom()), QVector2D(1.0f, 1.0f)},
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
        const qreal magnification = cursorMagnification();

        // Use the magnification from the last frame for the old rect, so
        // that if the magnification changed (shake started or ended), the
        // correct region is repainted.
        const QSizeF baseSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
        const QSizeF oldSize = baseSize * m_lastMagnification;
        const QSizeF newSize = baseSize * magnification;

        const QPointF oldHotspot = cursor.hotSpot() * m_lastMagnification;
        const QPointF newHotspot = cursor.hotSpot() * magnification;

        const QRect oldRect = QRectF(old - oldHotspot, oldSize).toAlignedRect();
        const QRect newRect = QRectF(pos - newHotspot, newSize).toAlignedRect();

        effects->addRepaint(KWin::Rect(oldRect));
        effects->addRepaint(KWin::Rect(newRect));
    }
}

} // namespace KWin
#include "moc_xorcursor.cpp"
