/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glframebuffer.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"
#include "opengl/glvertexbuffer.h"

#include <epoxy/gl.h>

#include <array>

namespace KWin
{

InvertCursorEffect::InvertCursorEffect()
{
    hideCursor();
}

InvertCursorEffect::~InvertCursorEffect()
{
    showCursor();
}

GLTexture *InvertCursorEffect::ensureCursorTexture()
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

void InvertCursorEffect::markCursorTextureDirty()
{
    m_cursorTextureDirty = true;
}

GLShader *InvertCursorEffect::ensureShader()
{
    if (!m_shader) {
        // The fragment shader lives in this plugin's Qt resource (see
        // qt_add_resources in src/CMakeLists.txt). ShaderManager resolves
        // its `#include "colormanagement.glsl"` from kwin's own shaders
        // resource at compile time, exactly like the built-in invert effect
        // does. Passing an empty vertex source makes ShaderManager generate
        // the standard transform vertex shader from the given traits, which
        // also declares all colormanagement uniforms our fragment shader
        // consumes and provides the `texcoord0` varying.
        auto s = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace,
                                                                  QByteArray(),
                                                                  QByteArrayLiteral(":/effects/cursor_invert/shaders/cursor_invert.frag"));
        if (!s || !s->isValid()) {
            return nullptr;
        }
        m_backgroundSamplerLocation = s->uniformLocation("backgroundSampler");
        m_cursorSamplerLocation = s->uniformLocation("cursorSampler");
        m_shader = std::move(s);
    }
    return m_shader.get();
}

void InvertCursorEffect::ensureGrabber(const QSize &size)
{
    // The invert shader needs the already-composited screen content as a
    // texture it can sample from. A grabber FBO does exactly that: blitting
    // the currently bound framebuffer into it copies everything we have
    // painted so far this frame into the grabber's color attachment (see
    // GLFramebuffer::blitFromRenderTarget()).
    if (!m_grabberFbo || m_grabberSize != size) {
        m_grabberFbo.reset();
        m_grabberTexture.reset();
        m_grabberSize = QSize();
        if (size.isEmpty()) {
            return;
        }
        m_grabberTexture = GLTexture::allocate(GL_RGBA8, size);
        if (!m_grabberTexture) {
            return;
        }
        m_grabberTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_grabberFbo = std::make_unique<GLFramebuffer>(m_grabberTexture.get());
        if (!m_grabberFbo->valid()) {
            // GLFramebuffer doesn't take ownership of a broken attachment
            // either way; drop both and retry on the next paint pass.
            m_grabberFbo.reset();
            m_grabberTexture.reset();
            return;
        }
        m_grabberSize = size;
    }
}

void InvertCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &InvertCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged, this, &InvertCursorEffect::slotMouseChanged);
        // repaint the last painted cursor area so that any inverted pixels
        // are restored to the original content before KWin starts drawing
        // its own (non-inverting) cursor again
        if (!m_cursorPaintRect.isEmpty()) {
            const QRect r = m_cursorPaintRect.toAlignedRect();
            effects->addRepaint(r.x(), r.y(), r.width(), r.height());
        }
        // show the previously hidden mouse-pointer and free the loaded resources.
        effects->showCursor();
        m_cursorTexture.reset();
        m_grabberFbo.reset();
        m_grabberTexture.reset();
        m_grabberSize = QSize();
        m_shader.reset();
        m_cursorPaintRect = QRectF();
        m_isMouseHidden = false;
    }
}

void InvertCursorEffect::hideCursor()
{
    if (!m_isMouseHidden) {
        // try to load the cursor-theme into a OpenGL texture and if successful then hide the mouse-pointer
        GLTexture *texture = nullptr;
        if (effects->isOpenGLCompositing()) {
            texture = ensureCursorTexture();
        }
        if (texture) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &InvertCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged, this, &InvertCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
            // force an initial paint of the cursor at its current position
            effects->addRepaintFull();
        }
    }
}

QRectF InvertCursorEffect::cursorRectLogical() const
{
    const auto cursor = effects->cursorImage();
    const QSizeF size = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    return QRectF(p, size);
}

void InvertCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, Output *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, region, screen);
    if (!m_isMouseHidden) {
        return;
    }
    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return;
    }
    GLShader *shader = ensureShader();
    if (!shader) {
        return;
    }

    const QSizeF cursorSize = cursorRectLogical().size();
    const QPointF p = cursorRectLogical().topLeft();
    const auto scale = viewport.scale();

    // remember where we painted so that the same area can be restored later
    m_cursorPaintRect = QRectF(p, cursorSize);

    // Only paint if the cursor actually intersects the region being repainted
    // in this pass. Painting outside of the dirty region would leave the
    // inverted state inconsistent (the previous inversion could not be
    // undone by the next frame), which is exactly what this check prevents.
    const QRectF cursorDeviceRect = scaledRect(m_cursorPaintRect, scale);
    if (!region.intersects(cursorDeviceRect.toAlignedRect())) {
        return;
    }

    // Grab the composited screen content so that the shader can read back
    // the pixels underneath the cursor. This replaces the old fixed-function
    // GL_COLOR_LOGIC_OP/GL_XOR approach with a modern shader pass.
    const QSize grabSize = renderTarget.size() * scale;
    ensureGrabber(grabSize);
    if (!m_grabberFbo) {
        return;
    }
    // Blit the dirty region of the framebuffer effects->paintScreen() just
    // rendered into (the currently bound one) onto the identically sized
    // area of the grabber's color attachment, then restore the render
    // target as the drawing destination.
    GLFramebuffer::pushFramebuffer(m_grabberFbo.get());
    m_grabberFbo->blitFromRenderTarget(renderTarget, viewport, region.boundingRect(), region.boundingRect());
    GLFramebuffer::popFramebuffer();

    ShaderManager::instance()->pushShader(shader);

    // The grabbed image holds the framebuffer's encoded values; run them
    // through the colorspace conversion uniforms so the inversion happens
    // in linear light (src == dst for the usual case, but the nits-based
    // round trip keeps HDR/wide-gamut outputs correct).
    shader->setColorspaceUniforms(renderTarget.colorDescription(), renderTarget.colorDescription(), RenderingIntent::Perceptual);

    // Draw the cursor quad directly in device coordinates via an ortho
    // projection, because the fragment shader samples the backdrop through
    // gl_FragCoord and needs the quad mapped one-to-one onto the framebuffer.
    QMatrix4x4 mat;
    mat.ortho(QRectF(QPointF(), QSizeF(grabSize)));
    mat.scale(1, -1); // undo ortho()'s y flip so top-left origin matches the viewport convention
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mat);

    // unit 0: freshly grabbed background, unit 1: the cursor shape
    glActiveTexture(GL_TEXTURE0);
    m_grabberTexture->bind();
    shader->setUniform(m_backgroundSamplerLocation, 0);
    glActiveTexture(GL_TEXTURE1);
    cursorTexture->bind();
    shader->setUniform(m_cursorSamplerLocation, 1);

    // Unlike XOR, the inversion is performed entirely inside the fragment
    // shader and fully masked by the cursor's alpha channel, so no special
    // blending or logic ops are required and any cursor colour works.
    std::array<GLVertex2D, 6> quad = {{
        // first triangle
        GLVertex2D{QVector2D(cursorDeviceRect.left(), cursorDeviceRect.top()), QVector2D(0.0, 1.0)},
        GLVertex2D{QVector2D(cursorDeviceRect.right(), cursorDeviceRect.bottom()), QVector2D(1.0, 0.0)},
        GLVertex2D{QVector2D(cursorDeviceRect.left(), cursorDeviceRect.bottom()), QVector2D(0.0, 0.0)},
        // second triangle
        GLVertex2D{QVector2D(cursorDeviceRect.left(), cursorDeviceRect.top()), QVector2D(0.0, 1.0)},
        GLVertex2D{QVector2D(cursorDeviceRect.right(), cursorDeviceRect.top()), QVector2D(1.0, 1.0)},
        GLVertex2D{QVector2D(cursorDeviceRect.right(), cursorDeviceRect.bottom()), QVector2D(1.0, 0.0)},
    }};

    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setVertices(quad);
    vbo->render(GL_TRIANGLES);

    glActiveTexture(GL_TEXTURE0);
    ShaderManager::instance()->popShader();
}

bool InvertCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void InvertCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (pos == old) {
        return;
    }
    // Both the area where the cursor was previously painted and the area
    // where it will be painted now have to be repainted: the old one to
    // restore the original pixels (they are currently inverted), the new one
    // to invert them. Repainting only these two rectangles instead of the
    // whole screen massively reduces the amount of work per frame.
    auto addLogicRectRepaint = [this](const QRectF &rect) {
        const QRect r = rect.toAlignedRect();
        effects->addRepaint(r.x(), r.y(), r.width(), r.height());
    };
    if (!m_cursorPaintRect.isEmpty()) {
        addLogicRectRepaint(m_cursorPaintRect);
    }
    addLogicRectRepaint(cursorRectLogical());
}

} // namespace KWin

#include "moc_xorcursor.cpp"
