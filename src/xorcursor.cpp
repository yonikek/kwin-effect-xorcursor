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
#include <QRegion>

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

        // True when the bound framebuffer stores colours in a fixed-point format. Such a
        // buffer clamps negative blend results to 0 by itself, so the clamp pass is not
        // needed. Anything unknown (default framebuffer, failed query) returns false.
        bool boundFramebufferIsFixedPoint()
        {
            GLint fbo = 0;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
            if (fbo == 0) {
                return false;
            }
            GLint type = 0;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                  GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &type);
            return type == GL_UNSIGNED_NORMALIZED;
        }

        // Schedules a repaint of a single rect. EffectsHandler::addRepaint() has taken
        // different rect types over the KWin versions, so use whichever overload exists
        // and fall back to a full repaint (correct, just slower) if none matches.
        template<typename Handler>
        auto repaintImpl(Handler *handler, const QRect &r, int) -> decltype(handler->addRepaint(Rect(r)), void())
        {
            handler->addRepaint(Rect(r));
        }

        template<typename Handler>
        auto repaintImpl(Handler *handler, const QRect &r, long) -> decltype(handler->addRepaint(r), void())
        {
            handler->addRepaint(r);
        }

        template<typename Handler>
        void repaintImpl(Handler *handler, const QRect &, ...)
        {
            handler->addRepaintFull();
        }

        void repaintRect(const QRect &r)
        {
            repaintImpl(effects, r, 0);
        }

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

        // Repaint only the area the cursor leaves and the area it enters. Several
        // events between two frames accumulate, so every intermediate position is
        // cleaned up. The area under the cursor is additionally recomposed on every
        // frame in prePaintScreen().
        connect(effects, &EffectsHandler::mouseChanged, this, [this]() {
            const QRect r = cursorRect();
            if (!m_lastRect.isEmpty()) {
                repaintRect(m_lastRect);
            }
            if (!r.isEmpty()) {
                repaintRect(r);
            }
            m_lastRect = r;
        });

        m_lastRect = cursorRect();
        if (!m_lastRect.isEmpty()) {
            repaintRect(m_lastRect);
        }
    }

    XorCursorEffect::~XorCursorEffect()
    {
        effects->showCursor();
        // Remove our drawing from the screen.
        if (!m_lastRect.isEmpty()) {
            repaintRect(m_lastRect);
        }
    }

    QRect XorCursorEffect::cursorRect() const
    {
        const PlatformCursorImage cursor = effects->cursorImage();
        const QImage image = cursor.image();
        if (image.isNull()) {
            return QRect();
        }
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        const QRectF rect(effects->cursorPos() - cursor.hotSpot(), QSizeF(image.size()) / dpr);
        return rect.toAlignedRect().adjusted(-1, -1, 1, 1);
    }

    void XorCursorEffect::prePaintScreen(ScreenPrePaintData &data)
    {
        // The cursor is drawn by inverting what is already in the buffer, which is not
        // idempotent: if a frame didn't recompose the area under the cursor, the
        // previous frame's inversion would be inverted back. Including the cursor rect
        // in every frame's paint region prevents that, however small the frame's damage.
        const QRect r = cursorRect();
        if (!r.isEmpty()) {
            data.paint += Rect(r);
        }
        Effect::prePaintScreen(data);
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

    void XorCursorEffect::rebuildCursor(const QImage &source)
    {
        // Fully opaque pixels (alpha == 255) are the ones that invert. They are
        // collected as rects and removed from a copy of the cursor image, so that the
        // remaining, even slightly transparent, pixels can be drawn as the theme
        // intended.
        QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);

        m_opaqueRects.clear();
        m_scaledCache.clear();
        bool hasTranslucent = false;

        // Runs of opaque pixels with the same x and width in consecutive rows are
        // merged into one rect. This keeps the rect count (and so the number of draw
        // calls per frame) low without any quadratic region unions.
        QHash<quint64, int> open;
        QHash<quint64, int> next;
        for (int y = 0; y < image.height(); ++y) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            next.clear();
            int x = 0;
            while (x < image.width()) {
                if (qAlpha(line[x]) == 255) {
                    const int start = x;
                    while (x < image.width() && qAlpha(line[x]) == 255) {
                        line[x] = 0;
                        ++x;
                    }
                    const quint64 key = (quint64(start) << 32) | quint32(x - start);
                    const auto it = open.constFind(key);
                    if (it != open.constEnd()) {
                        m_opaqueRects[*it].setHeight(m_opaqueRects[*it].height() + 1);
                        next.insert(key, *it);
                    } else {
                        m_opaqueRects.append(QRect(start, y, x - start, 1));
                        next.insert(key, m_opaqueRects.size() - 1);
                    }
                } else {
                    hasTranslucent = hasTranslucent || qAlpha(line[x]) != 0;
                    ++x;
                }
            }
            std::swap(open, next);
        }

        if (hasTranslucent) {
            image.setDevicePixelRatio(source.devicePixelRatio());
            m_translucentTexture = GLTexture::upload(image);
            if (m_translucentTexture) {
                m_translucentTexture->setFilter(GL_LINEAR);
                m_translucentTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            }
        } else {
            m_translucentTexture.reset();
        }

        m_cacheKey = source.cacheKey();
    }

    const QList<QRect> &XorCursorEffect::scaledRects(qreal factor)
    {
        const int key = qRound(factor * 1024);
        auto it = m_scaledCache.find(key);
        if (it != m_scaledCache.end()) {
            return *it;
        }

        // Scale to device pixels. QRegion normalises its rects so they never overlap,
        // which matters because subtract blending would otherwise apply twice.
        QRegion region;
        for (const QRect &r : std::as_const(m_opaqueRects)) {
            region += QRectF(r.x() * factor, r.y() * factor,
                             r.width() * factor, r.height() * factor)
            .toAlignedRect();
        }
        QList<QRect> rects;
        for (const QRect &r : region) {
            rects.append(r);
        }
        // Cached per factor, so outputs with different scales don't rebuild every frame.
        return m_scaledCache.insert(key, rects).value();
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

        // Position of the cursor image relative to the viewport, in logical coordinates.
        const auto renderRect = viewport.renderRect();
        const QPointF pos = effects->cursorPos() - cursor.hotSpot() - renderRect.topLeft();
        const QSizeF cursorSize = QSizeF(image.size()) / dpr;

        // Nothing to do when the cursor is on another output.
        const QRectF viewportRect(0, 0, renderRect.width(), renderRect.height());
        if (!QRectF(pos, cursorSize).intersects(viewportRect)) {
            return false;
        }

        if (image.cacheKey() != m_cacheKey) {
            rebuildCursor(image);
        }
        const QList<QRect> &opaque = scaledRects(factor);
        const bool drawTranslucent = kDrawTranslucentParts && m_translucentTexture;
        if (opaque.isEmpty() && !drawTranslucent) {
            return false;
        }

        // Decided once per frame; cheap state queries.
        const bool needsClamp = !opaque.isEmpty() && !boundFramebufferIsFixedPoint();

        BlendStateGuard guard;
        glEnable(GL_BLEND);

        // One shader and one set of colourspace uniforms for all passes.
        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture
        | ShaderTrait::TransformColorspace);
        // A pure white texel is mapped to reference white in the render target's
        // colour description, which gives us W in buffer units for free.
        s->setColorspaceUniforms(ColorDescription::sRGB,
                                 renderTarget.colorDescription(),
                                 RenderingIntent::Perceptual);

        const QPointF devicePos = pos * scale;
        // One solid rectangle per rect of opaque cursor pixels. The texture is a single
        // solid texel, so texture coordinates don't matter. This avoids the Region-based
        // GLTexture::render() overloads, whose API changes between releases.
        auto drawRects = [&](GLTexture *texture) {
            for (const QRect &r : opaque) {
                QMatrix4x4 mvp = viewport.projectionMatrix();
                mvp.translate(devicePos.x() + r.x(), devicePos.y() + r.y());
                s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
                texture->render(QSizeF(r.size()));
            }
        };

        if (!opaque.isEmpty()) {
            // Pass 1: result = S - D, where S is white (W). Destination alpha is untouched.
            glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            drawRects(m_whiteTexture.get());

            // Pass 2: on float targets, pixels brighter than W went negative. MAX with
            // black clamps them to 0. MIN/MAX ignore the blend factors. Fixed-point
            // targets clamp on their own, so this pass is skipped for them.
            if (needsClamp) {
                glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
                drawRects(m_blackTexture.get());
            }
        }

        // Pass 3: draw the remaining (partly transparent) cursor pixels normally.
        // Set kDrawTranslucentParts to false to leave the screen untouched there.
        if (drawTranslucent) {
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);

            QMatrix4x4 mvp = viewport.projectionMatrix();
            mvp.translate(devicePos.x(), devicePos.y());
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
            m_translucentTexture->render(QSizeF(image.size()) * factor);
        }

        ShaderManager::instance()->popShader();
        return true;
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
