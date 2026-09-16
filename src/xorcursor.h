/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "core/colorspace.h"
#include "effect/effect.h"

#include <QImage>
#include <memory>

namespace KWin {

    class GLTexture;

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

    private:
        void showCursor();
        void hideCursor();
        GLTexture *ensureCursorTexture();
        void markCursorTextureDirty();

        void ensureBackgroundTexture(const QSize &deviceSize);

        std::unique_ptr<GLTexture> m_cursorTexture;
        bool m_cursorTextureDirty = false;
        bool m_isMouseHidden = false;

        std::unique_ptr<GLTexture> m_backgroundTexture;
        QSize m_backgroundTextureSize;

        QRect m_lastCursorRect;
    };

} // namespace KWin
