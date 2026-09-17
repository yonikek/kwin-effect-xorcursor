/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "xorcursor.h"

#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glshader.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/glvertexbuffer.h"

#include <QImage>
#include <QOpenGLContext>
#include <QRegularExpression>

#include <KConfigGroup>
#include <KSharedConfig>

#include <array>

namespace KWin {

    // Config group name. Must match the effect's "Id" in the .json metadata.
    static constexpr char kConfigGroup[]  = "Effect-xorcursor";
    static constexpr char kConfigSmooth[] = "SmoothCursor";

    // ---------------------------------------------------------------------------
    // GLSL version detection (only used once, at first paint).
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

// ---------------------------------------------------------------------------
// Fragment shaders.
//
// `1.0 - x` on a normalized 8-bit channel is bit-exact `x XOR 0xFF`.
// The sampler filter (NEAREST vs LINEAR) is set on the texture object, not
// in the shader, so this code is filter-agnostic.
// ---------------------------------------------------------------------------
static const char kShaderLegacy[] = R"(
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
uniform vec2 backgroundTexScale;
varying vec2 texcoord0;

void main()
{
    vec4 cursor = texture2D(sampler, texcoord0);
    vec4 bg = texture2D(backgroundTexture, texcoord0 * backgroundTexScale);
    vec3 inverted = vec3(1.0) - bg.rgb;
    gl_FragColor = vec4(inverted, cursor.a);
}
)";

static const char kShaderModern[] = R"(
#version 140
uniform sampler2D sampler;
uniform sampler2D backgroundTexture;
uniform vec2 backgroundTexScale;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 cursor = texture(sampler, texcoord0);
    vec4 bg = texture(backgroundTexture, texcoord0 * backgroundTexScale);
    vec3 inverted = vec3(1.0) - bg.rgb;
    fragColor = vec4(inverted, cursor.a);
}
)";

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

XorCursorEffect::XorCursorEffect()
{
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(0);
    connect(&m_repaintTimer, &QTimer::timeout,
            this, &XorCursorEffect::flushPendingRepaints);

    connect(effects, &EffectsHandler::cursorShapeChanged,
            this, &XorCursorEffect::slotCursorShapeChanged);
    connect(effects, &EffectsHandler::mouseChanged,
            this, &XorCursorEffect::slotMouseChanged);

    // KWin calls reconfigure(ReconfigureAll) once after loading the effect,
    // so m_smoothCursor will be populated before the first paint. We still
    // set a sane default here for the unlikely case that it isn't.
    m_smoothCursor = effects->config()
    ->group(QString::fromLatin1(kConfigGroup))
    .readEntry(kConfigSmooth, false);

    tryAcquireHide();
}

XorCursorEffect::~XorCursorEffect()
{
    m_repaintTimer.stop();
    disconnect(effects, &EffectsHandler::cursorShapeChanged,
               this, &XorCursorEffect::slotCursorShapeChanged);
    disconnect(effects, &EffectsHandler::mouseChanged,
               this, &XorCursorEffect::slotMouseChanged);
    releaseHide();
}

void XorCursorEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)

    const bool previous = m_smoothCursor;
    m_smoothCursor = effects->config()
    ->group(QString::fromLatin1(kConfigGroup))
    .readEntry(kConfigSmooth, false);

    if (m_smoothCursor != previous) {
        // Force the cursor texture to be re-uploaded with the new filter
        // and repaint the cursor region so the change is visible at once.
        invalidateCursorTexture();
        if (!m_lastCursorRect.isEmpty()) {
            effects->addRepaint(Rect(m_lastCursorRect));
        }
    }
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
    m_shader.reset();
    m_hideAcquired = false;
}

bool XorCursorEffect::isHiddenByOtherEffect()
{
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
                m_cursorTexture->setFilter(m_smoothCursor ? GL_LINEAR : GL_NEAREST);
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
    if (m_backgroundTexture
        && m_backgroundTextureSize.width() >= size.width()
        && m_backgroundTextureSize.height() >= size.height()) {
        return;
        }

        QSize newSize = size;
    if (m_backgroundTexture) {
        newSize.setWidth(qMax(newSize.width(), m_backgroundTextureSize.width()));
        newSize.setHeight(qMax(newSize.height(), m_backgroundTextureSize.height()));
    }

    m_backgroundTexture.reset();
    m_backgroundTextureSize = newSize;

    QImage dummy(newSize, QImage::Format_RGBA8888);
    dummy.fill(Qt::transparent);
    m_backgroundTexture = GLTexture::upload(dummy);
    if (m_backgroundTexture) {
        m_backgroundTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        // Always NEAREST: we sample the exact pixels behind the cursor,
        // any smoothing would mix in colours that were never there.
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
// Batched damage
// ---------------------------------------------------------------------------

void XorCursorEffect::queueDamage(const QRegion &damage)
{
    if (damage.isEmpty()) {
        return;
    }
    m_pendingDamage |= damage;
    if (!m_repaintScheduled) {
        m_repaintScheduled = true;
        m_repaintTimer.start();
    }
}

void XorCursorEffect::flushPendingRepaints()
{
    m_repaintScheduled = false;
    if (m_pendingDamage.isEmpty()) {
        return;
    }
    effects->addRepaint(Region(m_pendingDamage));
    m_pendingDamage = QRegion();
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
    tryAcquireHide();

    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);

    if (!m_hideAcquired) {
        return;
    }

    if (!effects->isCursorHidden()) {
        effects->hideCursor();
    }

    if (isHiddenByOtherEffect()) {
        if (!m_lastCursorRect.isEmpty()) {
            effects->addRepaint(Rect(m_lastCursorRect));
            m_lastCursorRect = QRect();
        }
        return;
    }

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

    if (!m_shader) {
        m_useModernGlsl = (parseGlslVersion() >= Version(1, 40));
        const QByteArray source = m_useModernGlsl
        ? QByteArray(kShaderModern)
        : QByteArray(kShaderLegacy);
        m_shader = ShaderManager::instance()->generateCustomShader(
            ShaderTrait::MapTexture, QByteArray(), source);
    }
    if (!m_shader) {
        qWarning() << "XorCursorEffect: custom shader unavailable";
        return;
    }

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint oldSrc = 0, oldDst = 0;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &oldSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &oldDst);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ShaderManager::instance()->pushShader(m_shader.get());
    m_shader->setUniform("sampler", 0);
    m_shader->setUniform("backgroundTexture", 1);
    m_shader->setUniform("backgroundTexScale",
                         QVector2D(float(deviceSize.width())  / m_backgroundTextureSize.width(),
                                   float(deviceSize.height()) / m_backgroundTextureSize.height()));

    glActiveTexture(GL_TEXTURE0);
    tex->bind();
    glActiveTexture(GL_TEXTURE1);
    m_backgroundTexture->bind();
    glActiveTexture(GL_TEXTURE0);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(pos.x() * scale, pos.y() * scale);
    m_shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

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
    if (!m_hideAcquired) {
        return;
    }

    const QRect newRect = cursorLogicalRect();
    queueDamage(QRegion(m_lastCursorRect) ^ QRegion(newRect));
}

void XorCursorEffect::slotCursorShapeChanged()
{
    invalidateCursorTexture();
    tryAcquireHide();
    if (!m_hideAcquired) {
        return;
    }

    QRegion damage;
    if (!m_lastCursorRect.isEmpty()) {
        damage |= QRegion(m_lastCursorRect);
    }
    const QRect newRect = cursorLogicalRect();
    if (!newRect.isEmpty()) {
        damage |= QRegion(newRect);
    }
    queueDamage(damage);
}

} // namespace KWin
#include "moc_xorcursor.cpp"
