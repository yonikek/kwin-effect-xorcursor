#pragma once

#include <effect/effect.h>

#include <QHash>
#include <QImage>
#include <QList>
#include <QRect>

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

        void prePaintScreen(ScreenPrePaintData &data) override;
        [[nodiscard]] bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                       int mask, const Region &deviceRegion, LogicalOutput *screen) override;

    private:
        // Area covered by the cursor in logical coordinates, with a 1px margin.
        QRect cursorRect() const;
        bool paintCursor(const RenderTarget &renderTarget, const RenderViewport &viewport);
        void rebuildCursor(const QImage &image);
        const QList<QRect> &scaledRects(qreal factor);

        std::unique_ptr<GLTexture> m_whiteTexture;
        std::unique_ptr<GLTexture> m_blackTexture;
        // The cursor with its fully opaque pixels removed (shadows, anti-aliased edges, ...).
        // Null when the cursor has no partly transparent pixels.
        std::unique_ptr<GLTexture> m_translucentTexture;

        // Fully opaque (alpha == 255) pixels of the cursor image as non-overlapping
        // rects, in image pixels
        QList<QRect> m_opaqueRects;
        // The same rects scaled to device pixels, cached per output scale factor
        QHash<int, QList<QRect>> m_scaledCache;
        qint64 m_cacheKey = 0;
        // Where the cursor was when we last scheduled a repaint
        QRect m_lastRect;
    };

} // namespace KWin
