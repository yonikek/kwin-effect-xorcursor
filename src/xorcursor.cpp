// SPDX-License-Identifier: GPL-2.0-or-later
#include "xorcursor.h"

#include <core/colorspace.h>
#include <core/output.h>
#include <core/rendertarget.h>
#include <core/renderviewport.h>
#include <effect/effecthandler.h>
#include <opengl/glshader.h>
#include <opengl/gltexture.h>
#include <opengl/glutils.h>

#include <KConfigGroup>
#include <KSharedConfig>

#include <QMatrix4x4>

#include <algorithm>

namespace KWin
{

    namespace
    {

        // Saves and restores the GL blend state this effect touches.
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
        // needed. Anything unknown (default framebuffer, failed query) returns false,
        // which keeps the clamp pass and is always correct.
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

        // Schedules a repaint of one rect. EffectsHandler::addRepaint() has taken different
        // rect types across KWin versions: use whichever overload exists and fall back to a
        // full repaint (correct, just slower) when none matches.
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

    } // namespace

    // ---------------------------------------------------------------------------
    // Lifecycle and configuration
    // ---------------------------------------------------------------------------

    XorCursorEffect::XorCursorEffect()
    {
        effects->makeOpenGLContextCurrent();
        m_whiteTexture = makeSolidTexture(Qt::white);
        m_blackTexture = makeSolidTexture(Qt::black);

        reconfigure(ReconfigureAll);

        if (!isActive()) {
            // Without our textures we couldn't draw a cursor, so leave the real one alone.
            return;
        }

        // Repaint only the areas the cursor leaves and enters. Events between two
        // frames accumulate, so every intermediate position is cleaned up.
        connect(effects, &EffectsHandler::mouseChanged, this, [this]() {
            scheduleRepaint(false);
        });
        // String-based so that a missing signal is a runtime warning, not a build error.
        connect(effects, SIGNAL(cursorShapeChanged()), this, SLOT(slotCursorShapeChanged()));

        refreshCursorImage();
        hideRealCursor();
        scheduleRepaint(true);
    }

    XorCursorEffect::~XorCursorEffect()
    {
        showRealCursor();
        // Remove our drawing from the screen.
        if (!m_lastRect.isEmpty()) {
            repaintRect(m_lastRect);
        }
        // The textures are released by the members' destructors; make sure the GL
        // context is current when that happens.
        effects->makeOpenGLContextCurrent();
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

    void XorCursorEffect::reconfigure(ReconfigureFlags)
    {
        // Options live in kwinrc under [Effect-xorcursor]:
        //   InvertThreshold      (1-255, default 255) pixels with alpha >= this invert
        //   DrawTranslucentParts (bool, default true)  draw the other pixels as themed
        const auto config = KSharedConfig::openConfig(QStringLiteral("kwinrc"));
        config->reparseConfiguration();
        const KConfigGroup group(config, QStringLiteral("Effect-xorcursor"));

        const int threshold = std::clamp(group.readEntry("InvertThreshold", 255), 1, 255);
        const bool translucent = group.readEntry("DrawTranslucentParts", true);

        const bool changed = !m_configLoaded
        || threshold != m_invertThreshold
        || translucent != m_drawTranslucent;

        m_invertThreshold = threshold;
        m_drawTranslucent = translucent;
        m_configLoaded = true;

        if (changed) {
            m_imageKey = 0; // forces the mask to be rebuilt with the new settings
            m_image = QImage();
            if (isActive() && m_cursorHidden) {
                refreshCursorImage();
                scheduleRepaint(true);
            }
        }
    }

    void XorCursorEffect::hideRealCursor()
    {
        // Balanced exactly once, so we never unhide a cursor somebody else hid.
        if (!m_cursorHidden) {
            effects->hideCursor();
            m_cursorHidden = true;
        }
    }

    void XorCursorEffect::showRealCursor()
    {
        if (m_cursorHidden) {
            effects->showCursor();
            m_cursorHidden = false;
        }
    }

    // ---------------------------------------------------------------------------
    // Cursor state and damage
    // ---------------------------------------------------------------------------

    bool XorCursorEffect::refreshCursorImage()
    {
        const PlatformCursorImage cursor = effects->cursorImage();
        const QImage image = cursor.image();
        if (image.isNull()) {
            m_hasCursor = false;
            return false;
        }

        const QPointF hotSpot = cursor.hotSpot();
        if (m_hasCursor && hotSpot == m_hotSpot) {
            if (image.cacheKey() == m_imageKey) {
                return true;
            }
            // Same pixels in a different QImage object: don't rebuild or re-upload.
            if (image.size() == m_image.size() && image == m_image) {
                m_imageKey = image.cacheKey();
                return true;
            }
        }

        m_hasCursor = true;
        m_image = image;
        m_imageKey = image.cacheKey();
        m_hotSpot = hotSpot;
        const qreal dpr = image.devicePixelRatio() > 0 ? image.devicePixelRatio() : 1.0;
        m_logicalSize = QSizeF(image.size()) / dpr;

        m_mask = XorCursorDetail::CursorMask::build(image, m_invertThreshold, m_drawTranslucent);
        m_rects.clear();
        m_restDirty = true; // the texture is (re)created while painting, with GL current
        return true;
    }

    QRect XorCursorEffect::damageRect() const
    {
        if (!m_hasCursor) {
            return QRect();
        }
        const QRectF rect(effects->cursorPos() - m_hotSpot, m_logicalSize);
        // Rounded outward, plus a margin for sub-pixel rounding at fractional scales.
        return rect.toAlignedRect().adjusted(-1, -1, 1, 1);
    }

    void XorCursorEffect::scheduleRepaint(bool force)
    {
        const QRect rect = damageRect();
        if (!force && rect == m_lastRect) {
            return;
        }
        if (!m_lastRect.isEmpty()) {
            repaintRect(m_lastRect);
        }
        if (!rect.isEmpty()) {
            repaintRect(rect);
        }
        m_lastRect = rect;
    }

    void XorCursorEffect::slotCursorShapeChanged()
    {
        refreshCursorImage();
        scheduleRepaint(true);
    }

    // ---------------------------------------------------------------------------
    // Painting
    // ---------------------------------------------------------------------------

    void XorCursorEffect::prePaintScreen(ScreenPrePaintData &data)
    {
        // The cursor is drawn by modifying what is already in the buffer, which is not
        // idempotent: a frame that doesn't recompose the area under the cursor would
        // invert the previous frame's result back. Putting the cursor rect into every
        // frame's paint region prevents that, however small the frame's own damage.
        if (refreshCursorImage()) {
            const QRect rect = damageRect();
            if (!rect.isEmpty()) {
                data.paint += Rect(rect);
            }
            // Also clean up where the cursor was, in case no event told us it moved.
            if (!m_lastRect.isEmpty() && m_lastRect != rect) {
                data.paint += Rect(m_lastRect);
            }
        }
        Effect::prePaintScreen(data);
    }

    bool XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                      int mask, const Region &deviceRegion, LogicalOutput *screen)
    {
        const bool ret = effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
        paintCursor(renderTarget, viewport);
        return ret;
    }

    const QList<QRect> &XorCursorEffect::scaledRects(const QSize &deviceSize)
    {
        const quint32 key = (quint32(deviceSize.width()) << 16) | quint32(deviceSize.height() & 0xffff);
        const auto it = m_rects.constFind(key);
        if (it != m_rects.constEnd()) {
            return *it;
        }
        return *m_rects.insert(key, m_mask.scaled(deviceSize));
    }

    void XorCursorEffect::ensureRestTexture()
    {
        if (!m_restDirty) {
            return;
        }
        m_restDirty = false;
        m_restTexture.reset();
        if (m_drawTranslucent && !m_mask.rest().isNull()) {
            m_restTexture = GLTexture::upload(m_mask.rest());
            if (m_restTexture) {
                m_restTexture->setFilter(GL_LINEAR);
                m_restTexture->setWrapMode(GL_CLAMP_TO_EDGE);
            }
        }
    }

    bool XorCursorEffect::paintCursor(const RenderTarget &renderTarget, const RenderViewport &viewport)
    {
        if (!m_hasCursor || !isActive()) {
            return false;
        }

        // Everything below is per output: its scale, colour description and
        // framebuffer format can all differ from the other outputs'.
        const qreal scale = viewport.scale();
        const auto renderRect = viewport.renderRect();

        // Snap to the device pixel grid so the mask and the image stay crisp and the
        // damage rect stays exact at fractional scales.
        const QPointF local = effects->cursorPos() - m_hotSpot - renderRect.topLeft();
        const QPoint devicePos(qRound(local.x() * scale), qRound(local.y() * scale));
        const QSize deviceSize(qRound(m_logicalSize.width() * scale),
                               qRound(m_logicalSize.height() * scale));
        if (deviceSize.isEmpty()) {
            return false;
        }

        // Nothing to draw when the cursor isn't on (or overlapping) this output.
        const QRect outputRect(0, 0, qRound(renderRect.width() * scale), qRound(renderRect.height() * scale));
        if (!QRect(devicePos, deviceSize).intersects(outputRect)) {
            return false;
        }

        ensureRestTexture();
        const bool invert = m_mask.hasInvertedPixels();
        const bool drawRest = m_restTexture != nullptr;
        if (!invert && !drawRest) {
            return false;
        }

        // Float buffers keep negative blend results, fixed-point ones clamp by themselves.
        const bool needsClamp = invert && !boundFramebufferIsFixedPoint();

        BlendStateGuard guard;
        glEnable(GL_BLEND);

        // One shader and one set of colourspace uniforms for all passes.
        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture
        | ShaderTrait::TransformColorspace);
        // A pure white texel is mapped to reference white in this render target's colour
        // description, which gives us W in the buffer's own units on SDR and HDR alike,
        // without assuming any particular encoding.
        s->setColorspaceUniforms(ColorDescription::sRGB,
                                 renderTarget.colorDescription(),
                                 RenderingIntent::Perceptual);
        QMatrix4x4 mvp = viewport.projectionMatrix();
        mvp.translate(devicePos.x(), devicePos.y());
        s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

        const QSizeF size(deviceSize);

        if (invert) {
            const QList<QRect> &rects = scaledRects(deviceSize);

            // One solid rectangle per rect of inverted pixels. The textures are a single
            // texel, so texture coordinates don't matter. (GLTexture::render(Region, size)
            // would batch these but does not clip to the region, so it is not used.)
            auto drawRects = [&](GLTexture *texture) {
                for (const QRect &r : rects) {
                    QMatrix4x4 rectMvp = mvp;
                    rectMvp.translate(r.x(), r.y());
                    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, rectMvp);
                    texture->render(QSizeF(r.size()));
                }
            };

            // Pass 1: result = W - D inside the mask. Destination alpha is untouched.
            glBlendEquationSeparate(GL_FUNC_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            drawRects(m_whiteTexture.get());

            // Pass 2 (float buffers only): pixels brighter than W went negative. MAX with
            // black clamps them to 0. MIN/MAX ignore the blend factors.
            if (needsClamp) {
                glBlendEquationSeparate(GL_MAX, GL_FUNC_ADD);
                glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
                drawRects(m_blackTexture.get());
            }

            // Pass 3 draws at the cursor origin again.
            s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
        }

        // Pass 3: the remaining, partly transparent pixels, drawn the way the theme made
        // them. Their texture has the inverted pixels cleared, so the passes never overlap.
        if (drawRest) {
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
            m_restTexture->render(size);
        }

        ShaderManager::instance()->popShader();
        return true;
    }

} // namespace KWin

#include "moc_xorcursor.cpp"
