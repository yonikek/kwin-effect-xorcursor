#pragma once

#include <effect/effect.h>

#include <QImage>
#include <QRegion>

#include <memory>

namespace KWin
{

    class GLTexture;

    class XorCursorEffect : public Effect
    {
        Q_OBJECT

    public:
        XorCursorEffect();
        ~XorCursorEffect() override;

        static bool supported();

        bool isActive() const override;
        int requestedEffectChainPosition() const override;

        [[nodiscard]] bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                       int mask, const Region &deviceRegion, LogicalOutput *screen) override;

    private:
        bool paintCursor(const RenderTarget &renderTarget, const RenderViewport &viewport);
        void rebuildCursor(const QImage &image, qreal factor);
        void drawRects(const RenderTarget &renderTarget, const RenderViewport &viewport,
                       GLTexture *texture, const QRegion &mask, const QPointF &pos);

        std::unique_ptr<GLTexture> m_whiteTexture;
        std::unique_ptr<GLTexture> m_blackTexture;
        // The cursor with its fully opaque pixels removed (shadows, anti-aliased edges, ...)
        std::unique_ptr<GLTexture> m_translucentTexture;

        // Fully opaque (alpha == 255) pixels of the cursor image, in image pixels
        QRegion m_opaquePixels;
        // The same region scaled to device pixels of the current output
        QRegion m_opaqueScaled;
        qint64 m_cacheKey = 0;
        qreal m_factor = 0;
    };

} // namespace KWin
