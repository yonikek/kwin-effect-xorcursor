#include "xorcursor.h"

#include <QPainter>
#include <QMatrix4x4>
#include <epoxy/gl.h>

using namespace KWin;

XorCursorEffect::XorCursorEffect()
: m_textureDirty(true)
{
    // Reload texture when the cursor shape changes
    connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::slotCursorShapeChanged);

    // Repaint optimization: only trigger repaints when the cursor actually moves
    connect(effects, &EffectsHandler::mouseChanged, this, [this](const QPoint &pos, const QPoint &oldpos, Qt::MouseButtons, Qt::KeyboardModifiers, Qt::KeyboardModifiers) {
        if (pos != oldpos) {
            slotCursorPosChanged();
        }
    });

    initShader();
}

XorCursorEffect::~XorCursorEffect()
{
}

void XorCursorEffect::initShader()
{
    // Custom fragment shader that forces the cursor to act as a pure white mask
    // based on its alpha channel, completely ignoring its original RGB colors.
    // This solves the "effect requires a white cursor" limitation.
    const QByteArray fragmentShader = QByteArrayLiteral(
        "uniform sampler2D sampler;\n"
        "varying vec2 texcoord0;\n"
        "void main() {\n"
        "    vec4 tex = texture2D(sampler, texcoord0);\n"
        "    // Output white color multiplied by the original alpha\n"
        "    // Opaque parts become (1,1,1,1), transparent parts become (0,0,0,0)\n"
        "    gl_FragColor = vec4(tex.a, tex.a, tex.a, tex.a);\n"
        "}\n"
    );

    m_shader.reset(ShaderManager::instance()->generateShaderFromCode(QByteArray(), fragmentShader));
}

void XorCursorEffect::slotCursorShapeChanged()
{
    m_textureDirty = true;
    effects->addRepaint(m_lastCursorRect);
}

void XorCursorEffect::slotCursorPosChanged()
{
    // Repaint optimization: request repaint for the old cursor position
    // and the new cursor position, rather than the whole screen.
    effects->addRepaint(m_lastCursorRect);

    QPoint hotspot = effects->cursorHotSpot();
    QPoint pos = effects->cursorPos();
    QSize size = effects->cursorImage().size();

    QRect newRect(pos - hotspot, size);
    effects->addRepaint(newRect);

    m_lastCursorRect = newRect;
}

bool XorCursorEffect::isActive() const
{
    return effects->isOpenGLCompositing();
}

void XorCursorEffect::updateTexture()
{
    if (!m_textureDirty) {
        return;
    }

    QImage cursorImage = effects->cursorImage();
    if (cursorImage.isNull()) {
        m_cursorTexture.reset();
        m_textureDirty = false;
        return;
    }

    m_cursorTexture.reset(new GLTexture(cursorImage));
    m_textureDirty = false;
}

void XorCursorEffect::prePaintScreen(ScreenPrePaintData &data, int time)
{
    // Add the current cursor rect to the paint region to ensure it repaints cleanly
    data.paint |= m_lastCursorRect;
    effects->prePaintScreen(data, time);
}

void XorCursorEffect::paintScreen(int mask, const QRegion &region, ScreenPaintData &data)
{
    // 1. Paint the rest of the screen first
    effects->paintScreen(mask, region, data);

    // 2. Overlay our shader-driven XOR cursor
    updateTexture();
    if (!m_cursorTexture || !m_shader) {
        return;
    }

    QPoint hotspot = effects->cursorHotSpot();
    QPoint pos = effects->cursorPos();
    QRect rect(pos - hotspot, m_cursorTexture->size());

    // Optimization: only render if the cursor is within the damaged region
    if (!region.intersects(rect)) {
        return;
    }

    // Save current blend state to avoid core-profile GL errors (no glPushAttrib)
    GLboolean blendEnabled;
    glGetBooleanv(GL_BLEND, &blendEnabled);
    GLint blendSrc, blendDst;
    glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrc);
    glGetIntegerv(GL_BLEND_DST_RGB, &blendDst);

    // Modern "XOR" equivalent using the Difference Blend Mode.
    // Because our shader forces Src to pure white based on alpha, this mathematically
    // forces Output = 1 - Dst, which is a perfect color inversion.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR);

    // Bind Shader & Projection Matrix
    ShaderBinder binder(m_shader.data());
    m_shader->setUniform(GLShader::ModelViewProjectionMatrix, data.projectionMatrix());

    m_cursorTexture->bind();

    // Standard KWin Quad drawing using VBOs
    GLVertexBuffer *vbo = GLVertexBuffer::streamingBuffer();
    vbo->reset();
    vbo->setUseColor(false);
    vbo->bindArrays();

    const float verts[] = {
        (float)rect.x(), (float)rect.y(),
        (float)rect.x() + rect.width(), (float)rect.y(),
        (float)rect.x(), (float)rect.y() + rect.height(),
        (float)rect.x() + rect.width(), (float)rect.y() + rect.height()
    };

    const float texcoords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f
    };

    vbo->setData(4, 2, verts, texcoords);
    vbo->draw(GL_TRIANGLE_STRIP, 0, 4);

    vbo->unbindArrays();
    m_cursorTexture->unbind();

    // Restore old blend state
    if (!blendEnabled) {
        glDisable(GL_BLEND);
    }
    glBlendFunc(blendSrc, blendDst);
}
