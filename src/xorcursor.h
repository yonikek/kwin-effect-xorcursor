/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "effect/effect.h"

#include <QPointF>
#include <QRect>
#include <QSize>

#include <memory>

namespace KWin {

    class GLTexture;

    /**
     * Draws the cursor as an inverting (XOR with all-ones) mask over the
     * composited scene, reproducing the classic X11 "XorCursor" behaviour.
     *
     * The cursor image itself contributes only its alpha channel: opaque pixels
     * invert the background, transparent pixels leave it untouched. This works
     * with any cursor theme, including dark outlines and multi-colour cursors,
     * because the cursor's RGB values are never read.
     */
    class XorCursorEffect : public Effect
    {
        Q_OBJECT

    public:
        XorCursorEffect();
        ~XorCursorEffect() override;

        void paintScreen(const RenderTarget &renderTarget,
                         const RenderViewport &viewport,
                         int mask,
                         const Region &deviceRegion,
                         LogicalOutput *screen) override;
                         bool isActive() const override;

    private Q_SLOTS:
        void slotMouseChanged(const QPointF &pos, const QPointF &old);
        void slotCursorShapeChanged();

    private:
        // Hide-state management
        void tryAcquireHide();
        void releaseHide();
        bool isHiddenByOtherEffect();

        // Cursor texture
        GLTexture *cursorTexture();
        void invalidateCursorTexture();

        // Background capture
        void ensureBackgroundTexture(const QSize &size);

        // Helpers
        QRect cursorLogicalRect() const;

        std::unique_ptr<GLTexture> m_cursorTexture;
        bool m_cursorTextureDirty = false;

        std::unique_ptr<GLTexture> m_backgroundTexture;
        QSize m_backgroundTextureSize;

        bool m_hideAcquired = false;
        QRect m_lastCursorRect;
    };

} // namespace KWin
