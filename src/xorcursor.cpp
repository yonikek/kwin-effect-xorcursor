#include "xorcursor.h"

#include <core/colorspace.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>
#include <opengl/glutils.h>

#include <QMatrix4x4>

namespace KWin
{

    namespace
    {

        // Saves and restores the blend state touched by this effect.
        class BlendStateGuard
        {
        public:
            BlendStateGuard()
            {
                m_wasEnabled = glIsEnabled(GL_BLEND);
                glGetIntegerv(GL_BLEND_SRC_RGB, &m_srcRgb);
                glGetIntegerv(GL_BLEND_DST_RGB, &m_dstRgb);
                glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_srcAlpha);
                glGetIntegerv(GL_BLEND_DST_ALPHA, &m_dstAlpha);
                glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_eqRgb);
                glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_eqAlpha);
            }

            ~BlendStateGuard()
            {
                glBlendEquationSeparate(m_eqRgb, m_eqAlpha);
                glBlendFuncSeparate(m_srcRgb, m_dstRgb, m_srcAlpha, m_dstAlpha);
                if (!m_wasEnabled) {
                    glDisable(GL_BLEND);
                }
            }

        private:
            GLboolean m_wasEnabled = GL_FALSE;
            GLint m_srcRgb = GL_ONE;
            GLint m_dstRgb = GL_ZERO;
            GLint m_srcAlpha = GL_ONE;
            GLint m_dstAlpha = GL_ZERO;
            GLint m_eqRgb = GL_FUNC_ADD;
            GLint m_eqAlpha = GL_FUNC_ADD;
        };

        std::unique_ptr<GLTexture> makeSolidTexture(const QColor &color)
        {
            QImage image(1, 1, QImage::Format_RGBA8888_Premultiplied);
            image.fill(color);
            auto texture = GLTexture::upload(image);
            if (texture) {
                texture->setFilter(GL_NEAREST);
                texture->setWrapMode(GL_CLAMP_TO_EDGE);
            }
            return texture;
        }

        // Draw the non-opaque cursor pixels as the theme intended instead of leaving them out.
        constexpr bool kDrawTranslucentParts = true;

    } // namespace

    XorCursorEffect::XorCursorEffect()
    {
        effects->makeOpenGLContextCurrent();
        m_whiteTexture = makeSolidTexture(Qt::white);
        m_blackTexture = makeSolidTexture(Qt::black);

        effects->hideCursor();

        // Repaint when the pointer moves or the cursor shape changes.
        connect(effects, &EffectsHandler::mouseChanged, this, []() {
            effects->addRepaintFull();
        });
    }

    XorCursorEffect::~XorCursorEffect()
    {
        effects->showCursor();
    }

    bool XorCursorEffect::supported()
    {
        return effects->isOpenGLCompositing();
    }

    bool XorCursorEffect::isActive() const
    {
        return m_whiteTexture && m_blackTexture;
    }

    int XorCursorEffect::requestedEffectChainPosition() const
    {
        // Late in the chain so the cursor ends up on top of everything.
        return 99;
    }

    bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                      int mask, const Region &deviceRegion, LogicalOutput *screen)
    {
        const bool ret = effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
        paintCursor(renderTarget, viewport);
        return ret;
    }

    void XorCursorEffect::rebuildCursor(const QImage &source, qreal factor)
    {
        const qint64 key = source.cacheKey();
        if (key == m_cacheKey && qFuzzyCompare(factor, m_factor)) {
            return;
        }

        if (key != m_cacheKey) {
            // Fully opaque pixels (alpha == 255) are the ones that invert. They are
            // collected into a region and removed from a copy of the cursor image, so
            // that the remaining, even slightly transparent, pixels can be drawn as
            // the cursor theme intended.
            QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            m_opaquePixels = QRegion();
            for (int y = 0; y < image.height(); ++y) {
                QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
                int runStart = -1;
                for (int x = 0; x <= image.width(); ++x) {
                    const bool opaque = x < image.width() && qAlpha(line[x]) == 255;
                    if (opaque) {
                        line[x] = 0;
                        if (runStart < 0) {
                            runStart = x;
                        }
                    } else if (runStart >= 0) {
                        m_opaquePixels += QRect(runStart, y, x - runStart, 1);
                        runStart = -1;
                    }
                }
            }

            image.setDevicePixelRatio(source.devicePixelRatio());
            m_translucentTexture = GLTexture::upload(image);
            if (m_translucentTexture) {
                m_translucentTexture->setFilter(GL_LINEAR);
                m_translucentTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            }
        }

        // Scale to device pixels. QRegion normalises its rects so they never overlap,
        // which matters because subtract blending would otherwise apply twice.
        m_opaqueScaled = QRegion();
        for (const QRect &r : m_opaquePixels) {
            m_opaqueScaled += QRectF(r.x() * factor, r.y() * factor,
                                     r.width() * factor, r.height() * factor)
            .toAlignedRect();
        }

        m_cacheKey = key;
        m_factor = factor;
    }

    void XorCursorEffect::drawRects(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                    GLTexture *texture, const QRegion &mask, const QPointF &pos)
    {
        if (mask.isEmpty()) {
            return;
        }

        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture
        | ShaderTrait::TransformColorspace);
        // A pure white texel is mapped to reference white in the render target's
        // colour description, which gives us W in buffer units for free.
        s->setColorspaceUniforms(ColorDescription::sRGB,
                                 renderTarget.colorDescription(),
                                 RenderingIntent::Perceptual);

        // One solid rectangle per run of opaque cursor pixels. The texture is a single
        // solid texel, so texture coordinates don't matter. This avoids the Region-based
        // GLTexture::render() overloads, whose API changes between releases.
        const qreal scale = viewport.scale();
        for (const QRect &r : mask) {
            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(pos.x() * scale + r.x(), pos.y() * scale + r.y());
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            texture->render(QSizeF(r.size()));
        }

        ShaderManager::instance()->popShader();
    }

    bool XorCursorEffect::paintCursor(const RenderTarget &renderTarget, const RenderViewport &viewport)
    {
        if (!m_whiteTexture || !m_blackTexture) {
            return false;
        }

        const PlatformCursorImage cursor = effects->cursorImage();
        const QImage image = cursor.image();
        if (image.isNull()) {
            return false;
        }

        const qreal scale = viewport.scale();
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        // image pixels -> output device pixels
        const qreal factor = scale / dpr;

        rebuildCursor(image, factor);

        // Position of the cursor image relative to the viewport, in logical coordinates.
        const QPointF pos = effects->cursorPos() - cursor.hotSpot()
        - viewport.renderRect().topLeft();

        BlendStateGuard guard;
        glEnable(GL_BLEND);

        if (!m_opaqueScaled.isEmpty()) {
            // Pass 1: result = S - D, where S is white (W). Destination alpha is untouched.
            glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            drawRects(renderTarget, viewport, m_whiteTexture.get(), m_opaqueScaled, pos);

            // Pass 2: pixels brighter than W went negative on float targets. MAX with
            // black clamps them to 0. MIN/MAX ignore the blend factors.
            glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            drawRects(renderTarget, viewport, m_blackTexture.get(), m_opaqueScaled, pos);
        }

        // Pass 3: draw the remaining (partly transparent) cursor pixels normally.
        // Set kDrawTranslucentParts to false to leave the screen untouched there.
        if (kDrawTranslucentParts && m_translucentTexture) {
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

            auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture
            | ShaderTrait::TransformColorspace);
            s->setColorspaceUniforms(ColorDescription::sRGB,
                                     renderTarget.colorDescription(),
                                     RenderingIntent::Perceptual);
            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(pos.x() * scale, pos.y() * scale);
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            m_translucentTexture->render(QSizeF(image.size()) * factor);
            ShaderManager::instance()->popShader();
        }

        return true;
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
