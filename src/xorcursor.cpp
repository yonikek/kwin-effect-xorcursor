/*
 S PDX-FileCopyrightText: *2025 Jin Liu <m.liu.jin@gmail.com>

 SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"
#include "opengl/glutils.h"

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
        if (m_xorShader || m_xorShaderFailed) {
            return;
        }

        // IMPORTANT: Do NOT include "#version 140" here.
        // generateCustomShader() prepends "#version 140" + TRAIT_* defines
        // before the custom source. A second #version causes a compile error,
        // which returns nullptr and crashes on setUniform.
        const QByteArray fragmentShader = QByteArrayLiteral(
            "uniform sampler2D sampler;\n"
            "uniform sampler2D screenSampler;\n"
            "in vec2 texcoord0;\n"
            "out vec4 fragColor;\n"
            "\n"
            "void main() {\n"
            "    vec4 cursor = texture(sampler, texcoord0);\n"
            "    vec4 bg = texture(screenSampler, texcoord0);\n"
            "\n"
            "    // Plasma invert mechanism: invert in gamma 2.2 space\n"
            "    vec3 bgRgb = bg.rgb / max(bg.a, 0.001);\n"
            "    bgRgb = pow(bgRgb, vec3(1.0 / 2.2));\n"
            "    bgRgb = vec3(1.0) - bgRgb;\n"
            "    bgRgb = pow(bgRgb, vec3(2.2));\n"
            "    bgRgb *= bg.a;\n"
            "\n"
            "    // Mix: where cursor is opaque show inverted bg, else original\n"
            "    vec3 result = mix(bg.rgb, bgRgb, cursor.a);\n"
            "    fragColor = vec4(result, 1.0);\n"
            "}\n"
        );

        m_xorShader = ShaderManager::instance()->generateCustomShader(
            ShaderTrait::MapTexture,
            QByteArray(), // vertex: use KWin's built-in
                                                                      fragmentShader
        );

        if (!m_xorShader) {
            m_xorShaderFailed = true;
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

        QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
        Region cursorRegion = Region(Rect(cursorDeviceRect.toAlignedRect()));

        // Repaint the cursor region so the background is fresh
        effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

        // Try the shader-based invert path
        bool useShader = false;
        GLTexture *screenTexture = renderTarget.texture();

        if (screenTexture) {
            ensureXorShader();
            useShader = (m_xorShader != nullptr);
        }

        if (useShader) {
            auto *shader = m_xorShader.get();
            ShaderManager::instance()->pushShader(shader);

            glActiveTexture(GL_TEXTURE1);
            screenTexture->bind();
            shader->setUniform("screenSampler", 1);
            glActiveTexture(GL_TEXTURE0);

            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(p.x() * scale, p.y() * scale);
            shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

            cursorTexture->render(cursorSize * scale);

            ShaderManager::instance()->popShader();

            glActiveTexture(GL_TEXTURE1);
            screenTexture->unbind();
            glActiveTexture(GL_TEXTURE0);
        } else {
            // Fallback: legacy glLogicOp path
            auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
            if (!s) {
                return;
            }
            s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);

            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(p.x() * scale, p.y() * scale);
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

            glEnable(GL_COLOR_LOGIC_OP);
            glLogicOp(GL_XOR);
            cursorTexture->render(cursorSize * scale);
            glDisable(GL_COLOR_LOGIC_OP);

            ShaderManager::instance()->popShader();
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
