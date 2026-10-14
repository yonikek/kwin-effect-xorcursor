/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "opengl/shader.h"
#include "opengl/shadermanager.h"

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

GLFramebuffer *InvertCursorEffect::ensureGrabberFbo(const QSize &size)
{
    // The invert shader needs the already-composited screen content as a
    // texture it can sample from. A grabber FBO does exactly that: blitting
    // the currently bound framebuffer into it copies everything we have
    // painted so far this frame into m_grabberFbo's color texture (see
    // GLFramebuffer::grab()).
    if (!m_grabberFbo || m_grabberFbo->size() != size) {
        m_grabberFbo.reset();
        if (size.isEmpty()) {
            return nullptr;
        }
        m_grabberFbo = std::make_unique<GLFramebuffer>(size);
        m_grabberFbo->attachColorTarget(make_unique<GLTexture>(GL_RGBA8, TextureTargetDefault), 0);
        if (!m_grabberFbo->isValid()) {
            m_grabberFbo.reset();
            return nullptr;
        }
        m_grabberFbo->colorTexture()->setWrapMode(GL_CLAMP_TO_EDGE);
    }
    return m_grabberFbo.get();
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
        // show the previously hidden mouse-pointer and free the loaded texture/FBO.
        effects->showCursor();
        m_cursorTexture.reset();
        m_grabberFbo.reset();
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

void InvertCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }
    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
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
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
    if (!deviceRegion.intersects(Rect(cursorDeviceRect.toAlignedRect()))) {
        return;
    }

    // Grab the composited screen content so that the shader can read back
    // the pixels underneath the cursor. This replaces the old fixed-function
    // GL_COLOR_LOGIC_OP/GL_XOR approach with a modern shader pass.
    const QSize grabSize = renderTarget.size() * scale;
    GLFramebuffer *grabber = ensureGrabberFbo(grabSize);
    if (!grabber) {
        return;
    }
    // Read from the currently bound framebuffer (the one effects->paintScreen()
    // just rendered into) and write into the grabber's color texture, then
    // restore the render target as the drawing destination.
    grabber->grab();

    // The vertex shader is KWin's standard TransformVertexShader (identical
    // to the one used by ShaderTrait::TransformTextureWithoutCoords); the
    // fragment shader implements the actual inversion (see
    // shaders/cursor_invert.fragment), and is compiled into this plugin's
    // Qt resource by src/CMakeLists.txt (see qt_add_resources there).
    static const QByteArray vertexSource = QByteArrayLiteral(R"(
        #version 140

        uniform mat4 ModelViewProjectionMatrix;

        in vec3 position;
        in vec2 texcoord0;

        out vec2 texcoord;

        void main()
        {
            gl_Position = ModelViewProjectionMatrix * vec4(position.x, position.y, 1.0);
            texcoord = texcoord0;
        }
    )");
    auto s = ShaderManager::instance()->pushShader(vertexSource, ":/kwin/effects/cursor_invert/shaders/cursor_invert.fragment");
    if (!s->isValid()) {
        ShaderManager::instance()->popShader();
        return;
    }

    // The grabbed image holds the framebuffer's encoded values interpreted as
    // unorms; sample it in the framebuffer colorspace and re-encode for the
    // render target we are drawing back into (usually the same).
    s->setColorspaceUniforms(renderTarget.colorDescription(), renderTarget.colorDescription(), RenderingIntent::Perceptual);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    // unit 0: freshly grabbed background, unit 1: the cursor shape
    s->setUniform("BackgroundTextureUnit", 0);
    s->setUniform("CursorTextureUnit", 1);
    grabber->colorTexture()->bind(0);
    cursorTexture->bind(1);

    // Unlike XOR, the inversion is performed entirely inside the fragment
    // shader and fully masked by the cursor's alpha channel, so no special
    // blending or logic ops are required and any cursor colour works.
    s->draw(cursorSize * scale);
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
