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
        void rebuildMask(const QImage &image, qreal factor);
        void drawMask(const RenderTarget &renderTarget, const RenderViewport &viewport,
                      GLTexture *texture, const QRegion &mask, const QPointF &pos);

        std::unique_ptr<GLTexture> m_whiteTexture;
        std::unique_ptr<GLTexture> m_blackTexture;

        // Opaque cursor pixels that are light (these invert the background) and
        // dark (these are drawn black), in image pixels
        QRegion m_lightPixels;
        QRegion m_darkPixels;
        // The same regions scaled to device pixels of the current output
        QRegion m_lightScaled;
        QRegion m_darkScaled;
        qint64 m_maskCacheKey = 0;
        qreal m_maskFactor = 0;
        QPointF m_hotSpot;
    };

} // namespace KWin
