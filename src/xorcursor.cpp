#include "xorcursor.h"

#include <kwinglplatform.h>
#include <kwingltexture.h>
#include <kwinglrenderer.h>
#include <kwineffects.h>
#include <kwinglutils.h>
#include <kwinconfig.h>

#include <QDebug>
#include <QFile>
#include <QPainter>
#include <QVector2D>

#include <KLocalizedString>

namespace KWin {

    static const char *kXorFragmentShader = R"(
#version 140

uniform sampler2D uCursorTexture;
uniform sampler2D uBackgroundTexture;
uniform vec2      uCursorPosition;   // cursor top-left in background texture coords
uniform vec2      uTextureSize;      // background texture size

in vec2 vTexCoord;
out vec4 fragColor;

// Convert a normalized float [0,1] to an 8-bit integer [0,255].
uint toByte(float v) {
    return uint(clamp(v, 0.0, 1.0) * 255.0 + 0.5);
}

// Convert an 8-bit integer [0,255] back to a normalized float [0,1].
float fromByte(uint v) {
    return float(v) / 255.0;
}

void main() {
    // Cursor sample (source).
    vec4 cursor = texture(uCursorTexture, vTexCoord);

    // Background sample (destination).
    vec2 bgCoord = (gl_FragCoord.xy - uCursorPosition) / uTextureSize;
    vec4 background = texture(uBackgroundTexture, bgCoord);

    // Bitwise XOR per colour channel.
    uint r = toByte(cursor.r) ^ toByte(background.r);
    uint g = toByte(cursor.g) ^ toByte(background.g);
    uint b = toByte(cursor.b) ^ toByte(background.b);

    // Preserve the cursor alpha so transparent areas stay transparent.
    fragColor = vec4(fromByte(r), fromByte(g), fromByte(b), cursor.a);
}
)";

XorCursorEffect::XorCursorEffect()
{
    reconfigure(ReconfigureAll);

    // Connect to cursor shape changes so the texture is refreshed.
    connect(effects, &EffectsHandler::cursorShapeChanged,
            this, &XorCursorEffect::markCursorTextureDirty);

    // The repaint timer coalesces rapid mouse movements.
    m_repaintTimer.setSingleShot(true);
    m_repaintTimer.setInterval(0);
    connect(&m_repaintTimer, &QTimer::timeout, this, [this]() {
        // The actual repaint is triggered by slotMouseChanged.
    });
}

XorCursorEffect::~XorCursorEffect()
{
    // Shader is owned by ShaderManager, no need to delete.
    m_renderTarget.reset();
    m_backgroundTexture.reset();
    m_cursorTexture.reset();
}

bool XorCursorEffect::supported()
{
    return effects->isOpenGLCompositing() && GLRenderTarget::supported();
}

void XorCursorEffect::reconfigure(ReconfigureFlags flags)
{
    Q_UNUSED(flags)
    m_enabled = true; // or read from config
    loadShader();
    if (m_enabled) {
        effects->hideCursor();
        m_isMouseHidden = true;
    }
}

bool XorCursorEffect::isActive() const
{
    return m_enabled && !m_isMouseHidden;
}

bool XorCursorEffect::isEnabled() const
{
    return m_enabled;
}

void XorCursorEffect::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    if (m_enabled) {
        hideCursor();
    } else {
        showCursor();
    }
    effects->addRepaintFull();
}

void XorCursorEffect::loadShader()
{
    if (m_xorShader && m_xorShader->isValid()) {
        return;
    }

    // Use ShaderManager to load a fragment shader with a built-in vertex shader.
    m_xorShader = ShaderManager::instance()->loadFragmentShader(
        ShaderManager::SimpleShader,
        QStringLiteral(":/effects/xorcursor/xorcursor.frag"));

    if (!m_xorShader || !m_xorShader->isValid()) {
        qWarning() << "XorCursorEffect: failed to load XOR fragment shader";
        m_enabled = false;
    }
}

void XorCursorEffect::updateCursorTexture()
{
    // Obtain the current cursor image (Xcursor).
    // This is a placeholder – replace with your existing Xcursor extraction code.
    xcb_xfixes_get_cursor_image_cookie_t cookie =
    xcb_xfixes_get_cursor_image_unchecked(xcbConnection());
    xcb_xfixes_get_cursor_image_reply_t *reply =
    xcb_xfixes_get_cursor_image_reply(xcbConnection(), cookie, nullptr);
    if (!reply) {
        return;
    }

    const uint32_t *pixels =
    xcb_xfixes_get_cursor_image_cursor_image(reply);
    const int width = reply->width;
    const int height = reply->height;
    const int xhot = reply->xhot;
    const int yhot = reply->yhot;

    // Convert ARGB32 to QImage (premultiplied as required by GL).
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint32_t pixel = pixels[y * width + x];
            const uint8_t a = (pixel >> 24) & 0xff;
            const uint8_t r = (pixel >> 16) & 0xff;
            const uint8_t g = (pixel >>  8) & 0xff;
            const uint8_t b =  pixel        & 0xff;
            image.setPixel(x, y, qRgba(r, g, b, a));
        }
    }
    free(reply);

    // Upload to GL texture.
    if (m_cursorTexture) {
        m_cursorTexture->update(image);
    } else {
        m_cursorTexture = std::make_unique<GLTexture>(image);
    }

    m_cursorImage = image;

    // Remember the hotspot for positioning.
    // Store it in a member if you need it elsewhere.
    // (Not shown here for brevity.)
}

void XorCursorEffect::markCursorTextureDirty()
{
    // The cursor shape changed – force a texture update on next paint.
    m_cursorTexture.reset();
    effects->addRepaintFull();
}

void XorCursorEffect::hideCursor()
{
    if (!m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged,
                   this, &XorCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged,
                   this, &XorCursorEffect::slotMouseChanged);
        effects->hideCursor();
        m_cursorTexture.reset();
        m_isMouseHidden = true;
    }
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged,
                   this, &XorCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged,
                   this, &XorCursorEffect::slotMouseChanged);
        effects->showCursor();
        m_cursorTexture.reset();
        m_isMouseHidden = false;
    }
}

bool XorCursorEffect::ensureRenderTarget(const QSize &size)
{
    if (m_renderTargetSize == size && m_renderTarget && m_renderTarget->valid()) {
        return true;
    }

    m_backgroundTexture.reset();
    m_renderTarget.reset();

    if (!GLRenderTarget::supported()) {
        return false;
    }

    m_backgroundTexture = std::make_unique<GLTexture>(GL_RGBA8, size);
    if (!m_backgroundTexture->isNull()) {
        m_backgroundTexture->setFilter(GL_LINEAR);
        m_backgroundTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_renderTarget = std::make_unique<GLRenderTarget>(*m_backgroundTexture);
        if (!m_renderTarget->valid()) {
            m_renderTarget.reset();
            m_backgroundTexture.reset();
            return false;
        }
    } else {
        m_backgroundTexture.reset();
        return false;
    }

    m_renderTargetSize = size;
    return true;
}

void XorCursorEffect::prePaintScreen(ScreenPrePaintData &data)
{
    Q_UNUSED(data)
    // Nothing special – the actual work happens in paintScreen.
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget,
                                  const RenderViewport &viewport,
                                  int mask,
                                  const QRegion &region,
                                  ScreenPaintData &data)
{
    // 1. Let the normal scene paint first – but we will re-render the
    //    cursor region into an offscreen texture and then composite it.
    effects->paintScreen(renderTarget, viewport, mask, region, data);

    if (!m_enabled || !m_xorShader || !m_xorShader->isValid()) {
        return;
    }

    // 2. Ensure the cursor texture is up to date.
    if (!m_cursorTexture) {
        updateCursorTexture();
    }
    if (!m_cursorTexture) {
        return; // no cursor image available
    }

    // 3. Determine the cursor geometry in logical coordinates.
    const QPointF cursorPos = effects->cursorPos();
    const QSize cursorSize = m_cursorImage.size() / m_cursorImage.devicePixelRatio();
    const QPointF hotspot = QPointF(0, 0); // Replace with actual hotspot.
    const QRectF logicalRect(cursorPos - hotspot, cursorSize);

    // 4. Convert to device coordinates using the viewport scale.
    const qreal scale = viewport.scale();
    const QRect deviceRect = QRectF(logicalRect.x() * scale,
                                    logicalRect.y() * scale,
                                    logicalRect.width() * scale,
                                    logicalRect.height() * scale).toAlignedRect();

                                    // 5. Build the region that needs repainting (union of old and new).
                                    const QRect oldDeviceRect = m_lastCursorRect.isNull()
                                    ? QRect()
                                    : QRectF(m_lastCursorRect.x() * m_lastScale,
                                             m_lastCursorRect.y() * m_lastScale,
                                             m_lastCursorRect.width() * m_lastScale,
                                             m_lastCursorRect.height() * m_lastScale).toAlignedRect();

                                             QRegion repaintRegion;
                                             if (!oldDeviceRect.isNull()) {
                                                 repaintRegion += oldDeviceRect;
                                             }
                                             repaintRegion += deviceRect;

                                             // 6. Render the background (scene without cursor) into the offscreen texture.
                                             if (!ensureRenderTarget(deviceRect.size())) {
                                                 return;
                                             }

                                             // Push the render target and re-render the scene into it.
                                             effects->pushRenderTarget(m_renderTarget.get());
                                             // Adjust the viewport so the scene is rendered at the correct offset.
                                             // This is a simplified version – you may need to use viewport.renderRect()
                                             // and set the projection matrix accordingly.
                                             effects->paintScreen(renderTarget, viewport, mask, deviceRect, data);
                                             effects->popRenderTarget();

                                             // 7. Apply the XOR shader to the cursor region.
                                             applyXorShader(deviceRect, deviceRect, cursorPos);

                                             // 8. Remember the cursor rect for the next frame.
                                             m_lastCursorRect = logicalRect.toAlignedRect();
                                             m_lastScale = scale;

                                             // 9. Request repaint of the affected region.
                                             effects->addRepaint(repaintRegion);
}

void XorCursorEffect::applyXorShader(const QRect &deviceRect,
                                     const QRect &cursorRect,
                                     const QPointF &cursorHotspot)
{
    if (!m_xorShader || !m_xorShader->isValid()) {
        return;
    }

    ShaderBinder binder(m_xorShader);

    // Bind the cursor texture.
    m_cursorTexture->bind();

    // Bind the background texture.
    m_backgroundTexture->bind();

    // Set uniforms.
    m_xorShader->setUniform("uCursorTexture", 0);
    m_xorShader->setUniform("uBackgroundTexture", 1);
    m_xorShader->setUniform("uCursorPosition",
                            QVector2D(deviceRect.x(), deviceRect.y()));
    m_xorShader->setUniform("uTextureSize",
                            QVector2D(m_renderTargetSize.width(),
                                      m_renderTargetSize.height()));

    // Draw a textured quad covering the cursor rect.
    // KWin's GLVertexBuffer can be used, or you can use a simple
    // full-screen quad with the appropriate projection.
    // For brevity, we assume a helper that draws a quad at the given rect.
    // (See the existing effect for the quad-drawing code.)

    m_backgroundTexture->unbind();
    m_cursorTexture->unbind();
}

void XorCursorEffect::postPaintScreen()
{
    // Nothing to do – the repaint was already requested in paintScreen.
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos,
                                       const QPointF &oldpos,
                                       Qt::MouseButtons buttons,
                                       Qt::MouseButtons oldbuttons,
                                       Qt::KeyboardModifiers modifiers,
                                       Qt::KeyboardModifiers oldmodifiers)
{
    Q_UNUSED(pos)
    Q_UNUSED(oldpos)
    Q_UNUSED(buttons)
    Q_UNUSED(oldbuttons)
    Q_UNUSED(modifiers)
    Q_UNUSED(oldmodifiers)

    if (!m_enabled || !m_isMouseHidden) {
        return;
    }

    // Ask for a repaint – the actual work happens in paintScreen.
    effects->addRepaintFull();
}

} // namespace KWin
