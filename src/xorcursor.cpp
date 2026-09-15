#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(KWIN_XOR_CURSOR, "kwin_effect_xorcursor", QtWarningMsg)

namespace KWin
{

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
            disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
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
                connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
                connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
                m_isMouseHidden = true;
            }
        }
    }

    void XorCursorEffect::ensureXorShader()
    {
        if (m_xorShader) return;

        const QString vertexShader = R"(
        #version 140
        uniform mat4 modelViewProjectionMatrix;
        in vec4 vertex;
        in vec2 texcoord;
        out vec2 texcoord0;
        void main() {
            gl_Position = modelViewProjectionMatrix * vertex;
            texcoord0 = texcoord;
        }
    )";

    const QString fragmentShader = R"(
        #version 140
        uniform sampler2D sampler;
        uniform sampler2D screenSampler;
        in vec2 texcoord0;
        out vec4 fragColor;

        void main() {
            vec4 cursor = texture(sampler, texcoord0);
            vec4 screen = texture(screenSampler, texcoord0);

            vec3 originalScreen = screen.rgb;

            // Plasma invert mechanism: 1.0 - rgb in gamma 2.2 space
            vec3 screenRgb = screen.rgb / max(0.001, screen.a);
            screenRgb = pow(screenRgb, vec3(1.0 / 2.2)); // Linear to Gamma 2.2
            screenRgb = vec3(1.0) - screenRgb;          // Invert
            screenRgb = pow(screenRgb, vec3(2.2));      // Gamma 2.2 to Linear
            screenRgb *= screen.a;

            // XOR logic: If cursor is opaque, show inverted background; otherwise show original background
            vec3 finalColor = mix(originalScreen, screenRgb, cursor.a);
            fragColor = vec4(finalColor, 1.0);
        }
    )";

    m_xorShader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        vertexShader.toUtf8(),
                                                                  fragmentShader.toUtf8()
    );

    if (!m_xorShader) {
        qCWarning(KWIN_XOR_CURSOR) << "Failed to compile XOR cursor shader!";
    }
    }

    void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
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

        QSize cursorDeviceSize = (cursorSize * scale).toSize();
        if (cursorDeviceSize.isEmpty()) {
            return;
        }

        QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
        Region cursorRegion = Region(Rect(cursorDeviceRect.toAlignedRect()));
        effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

        GLTexture *screenTexture = renderTarget.texture();
        bool useShader = (screenTexture != nullptr);

        if (useShader) {
            ensureXorShader();
            if (!m_xorShader) {
                qCWarning(KWIN_XOR_CURSOR) << "Shader failed to compile, falling back to glLogicOp";
                useShader = false;
            }
        }

        if (useShader) {
            // Create/resize background texture to match cursor size
            if (!m_cursorBgTexture || m_cursorBgTexture->size() != cursorDeviceSize) {
                m_cursorBgTexture = std::make_unique<GLTexture>(GL_RGBA8, cursorDeviceSize, GL_RGBA);
                m_cursorBgTexture->setFilter(GL_LINEAR);
                m_cursorBgTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            }

            // Copy the background from the framebuffer to avoid sampling the active render target (feedback loop)
            m_cursorBgTexture->bind();
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                                p.x() * scale, p.y() * scale,
                                cursorDeviceSize.width(), cursorDeviceSize.height());
            GLenum err = glGetError();
            m_cursorBgTexture->unbind();

            if (err != GL_NO_ERROR) {
                qCWarning(KWIN_XOR_CURSOR) << "glCopyTexSubImage2D failed with error" << err << ", falling back";
                useShader = false;
            }
        }

        GLShader *shader = nullptr;
        if (useShader) {
            shader = m_xorShader.get();
            ShaderManager::instance()->pushShader(shader);

            glActiveTexture(GL_TEXTURE1);
            m_cursorBgTexture->bind();
            shader->setUniform("screenSampler", 1);
            shader->setUniform("sampler", 0);

            glActiveTexture(GL_TEXTURE0);
        } else {
            auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
            if (!s) {
                qCWarning(KWIN_XOR_CURSOR) << "Failed to push fallback shader!";
                return;
            }
            shader = s;
            s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
            glEnable(GL_COLOR_LOGIC_OP);
            glLogicOp(GL_XOR);
        }

        if (!shader) {
            qCWarning(KWIN_XOR_CURSOR) << "Shader pointer is null after setup!";
            return;
        }

        QMatrix4x4 mvp = viewport.projectionMatrix();
        mvp.translate(p.x() * scale, p.y() * scale);
        shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

        cursorTexture->render(cursorSize * scale);

        ShaderManager::instance()->popShader();

        if (useShader) {
            glActiveTexture(GL_TEXTURE1);
            m_cursorBgTexture->unbind();
            glActiveTexture(GL_TEXTURE0);
        } else {
            glDisable(GL_COLOR_LOGIC_OP);
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
            QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();

            QRect newRect = QRectF(pos - cursor.hotSpot(), cursorSize).toAlignedRect();

            effects->addRepaint(KWin::Rect(m_lastCursorRect));
            effects->addRepaint(KWin::Rect(newRect));
        }
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
