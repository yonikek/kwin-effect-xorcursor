#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glutils.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/glframebuffer.h"
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

        // Fragment shader only: KWin will automatically prepend base.vert
        // which correctly binds "position" and "texcoord" attributes.
        const QString fragmentShader = R"(
        #version 140
        uniform sampler2D sampler;
        uniform sampler2D bgSampler;
        in vec2 texcoord0;
        out vec4 fragColor;

        void main() {
            vec4 cursor = texture(sampler, texcoord0);
            vec4 bg = texture(bgSampler, texcoord0);

            // Plasma invert mechanism: 1.0 - rgb in gamma 2.2 space
            vec3 bgRgb = bg.rgb / max(0.001, bg.a);
            bgRgb = pow(bgRgb, vec3(1.0 / 2.2)); // Linear to Gamma 2.2
            bgRgb = vec3(1.0) - bgRgb;          // Invert
            bgRgb = pow(bgRgb, vec3(2.2));      // Gamma 2.2 to Linear
            bgRgb *= bg.a;

            // XOR logic: If cursor is opaque, show inverted background; otherwise show original background
            vec3 finalColor = mix(bg.rgb, bgRgb, cursor.a);
            fragColor = vec4(finalColor, 1.0);
        }
    )";

    m_xorShader = ShaderManager::instance()->generateCustomShader(
        ShaderTrait::MapTexture,
        QByteArray(), // Use KWin's default base.vert
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
            // Allocate background texture using KWin's API
            if (!m_cursorBgTexture || m_cursorBgTexture->size() != cursorDeviceSize) {
                m_cursorBgTexture = GLTexture::allocate(GL_RGBA8, cursorDeviceSize);
                if (!m_cursorBgTexture) {
                    qCWarning(KWIN_XOR_CURSOR) << "Failed to allocate background texture, falling back";
                    useShader = false;
                } else {
                    m_cursorBgTexture->setFilter(GL_LINEAR);
                    m_cursorBgTexture->setWrapMode(GL_CLAMP_TO_EDGE);
                }
            }

            if (useShader) {
                // Safely copy background pixels using KWin's blit API
                // This handles all rotations, scaling, and Y-inversions automatically
                GLFramebuffer tempFbo(m_cursorBgTexture.get());
                bool blitSuccess = tempFbo.blitFromRenderTarget(renderTarget, viewport, Rect(cursorDeviceRect), Rect(QPoint(0,0), cursorDeviceSize));
                if (!blitSuccess) {
                    qCWarning(KWIN_XOR_CURSOR) << "Blit failed, falling back to glLogicOp";
                    useShader = false;
                }
            }
        }

        GLShader *shader = nullptr;
        if (useShader) {
            shader = m_xorShader.get();
            ShaderManager::instance()->pushShader(shader);

            glActiveTexture(GL_TEXTURE1);
            m_cursorBgTexture->bind();
            shader->setUniform("bgSampler", 1);

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
