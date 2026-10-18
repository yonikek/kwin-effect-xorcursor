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

    void XorCursorEffect::rebuildMask(const QImage &source, qreal factor)
    {
        const qint64 key = source.cacheKey();
        if (key == m_maskCacheKey && qFuzzyCompare(factor, m_maskFactor)) {
            return;
        }

        if (key != m_maskCacheKey) {
            // Split the opaque pixels (alpha >= 50%) by brightness: light pixels
            // invert the background, dark pixels stay black. This mirrors what
            // S*(1-D) + D*(1-a) did for black and white cursors.
            const QImage image = source.convertToFormat(QImage::Format_ARGB32);
            m_lightPixels = QRegion();
            m_darkPixels = QRegion();
            for (int y = 0; y < image.height(); ++y) {
                const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                // 0 = no run, 1 = light run, 2 = dark run
                int runType = 0;
                int runStart = 0;
                auto flush = [&](int endX) {
                    if (runType == 1) {
                        m_lightPixels += QRect(runStart, y, endX - runStart, 1);
                    } else if (runType == 2) {
                        m_darkPixels += QRect(runStart, y, endX - runStart, 1);
                    }
                };
                for (int x = 0; x < image.width(); ++x) {
                    int type = 0;
                    if (qAlpha(line[x]) >= 128) {
                        type = qGray(line[x]) >= 128 ? 1 : 2;
                    }
                    if (type != runType) {
                        flush(x);
                        runType = type;
                        runStart = x;
                    }
                }
                flush(image.width());
            }
        }

        // Scale to device pixels. QRegion normalises its rects so they never overlap,
        // which matters because subtract blending would otherwise apply twice.
        auto scaleRegion = [factor](const QRegion &in) {
            QRegion out;
            for (const QRect &r : in) {
                out += QRectF(r.x() * factor, r.y() * factor,
                              r.width() * factor, r.height() * factor)
                .toAlignedRect();
            }
            return out;
        };
        m_lightScaled = scaleRegion(m_lightPixels);
        m_darkScaled = scaleRegion(m_darkPixels);
        // Adjacent light/dark rects can overlap after rounding; light wins so the
        // inversion never gets painted over by black.
        m_darkScaled -= m_lightScaled;

        m_maskCacheKey = key;
        m_maskFactor = factor;
    }

    void XorCursorEffect::drawMask(const RenderTarget &renderTarget, const RenderViewport &viewport,
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

        // Draw one solid rectangle per run of cursor pixels. The texture is a
        // single solid texel, so texture coordinates don't matter. This avoids the
        // Region-based GLTexture::render() overloads, whose API changes between releases.
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

        rebuildMask(image, factor);
        if (m_lightScaled.isEmpty() && m_darkScaled.isEmpty()) {
            return false;
        }

        // Position of the cursor image relative to the viewport, in logical coordinates.
        const QPointF pos = effects->cursorPos() - cursor.hotSpot()
        - viewport.renderRect().topLeft();

        BlendStateGuard guard;
        glEnable(GL_BLEND);

        // Pass 1: dark cursor pixels are painted opaque black (replace the destination).
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);
        drawMask(renderTarget, viewport, m_blackTexture.get(), m_darkScaled, pos);

        // Pass 2: light cursor pixels give result = S - D, where S is white (W).
        // Destination alpha is left untouched.
        glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        drawMask(renderTarget, viewport, m_whiteTexture.get(), m_lightScaled, pos);

        // Pass 3: pixels brighter than W went negative in pass 2 on float targets.
        // MAX with black clamps them to 0. MIN/MAX ignore the blend factors.
        glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        drawMask(renderTarget, viewport, m_blackTexture.get(), m_lightScaled, pos);

        return true;
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
