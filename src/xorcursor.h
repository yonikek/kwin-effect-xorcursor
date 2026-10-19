// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cursormask.h"

#include <effect/effect.h>

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointF>
#include <QRect>
#include <QSizeF>

#include <memory>

namespace KWin
{

    class GLTexture;

    /**
     * Draws the mouse cursor so that its fully opaque pixels invert whatever is
     * underneath, on SDR and HDR outputs, on any number of outputs.
     */
    class XorCursorEffect : public Effect
    {
        Q_OBJECT

    public:
        XorCursorEffect();
        ~XorCursorEffect() override;

        static bool supported();

        bool isActive() const override;
        int requestedEffectChainPosition() const override;
        void reconfigure(ReconfigureFlags flags) override;

        void prePaintScreen(ScreenPrePaintData &data) override;
        [[nodiscard]] bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                                       int mask, const Region &deviceRegion, LogicalOutput *screen) override;

    private Q_SLOTS:
        void slotCursorShapeChanged();

    private:
        bool refreshCursorImage();
        QRect damageRect() const;
        void scheduleRepaint(bool force);
        void hideRealCursor();
        void showRealCursor();
        const QList<QRect> &scaledRects(const QSize &deviceSize);
        void ensureRestTexture();
        bool paintCursor(const RenderTarget &renderTarget, const RenderViewport &viewport);

        // --- configuration (kwinrc, group "Effect-xorcursor") ---
        int m_invertThreshold = 255;
        bool m_drawTranslucent = true;
        bool m_configLoaded = false;

        // --- cursor image state, refreshed only when the image changes ---
        bool m_hasCursor = false;
        QImage m_image;
        qint64 m_imageKey = 0;
        QPointF m_hotSpot;
        QSizeF m_logicalSize;
        XorCursorDetail::CursorMask m_mask;
        // Inverted pixels as device-pixel rects, per device size (one entry per output scale)
        QHash<quint32, QList<QRect>> m_rects;
        bool m_restDirty = true;

        // --- GL resources (created and destroyed with the GL context current) ---
        std::unique_ptr<GLTexture> m_whiteTexture;
        std::unique_ptr<GLTexture> m_blackTexture;
        std::unique_ptr<GLTexture> m_restTexture;

        // --- bookkeeping ---
        bool m_cursorHidden = false;
        // Where the cursor was when we last scheduled a repaint (logical coordinates)
        QRect m_lastRect;
    };

} // namespace KWin
