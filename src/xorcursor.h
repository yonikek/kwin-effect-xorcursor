/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Ide*ntifier: GPL-2.0-or-later */
#pragma once

#include "core/colorspace.h"
//#include "core/rect.h"
#include "effect/effect.h"

#include <QPointer>
#include <QRect>

#include <memory>

namespace KWin {

    class GLTexture;

    class XorCursorEffect : public Effect
    {
        Q_OBJECT
    public:
        XorCursorEffect();
        ~XorCursorEffect() override;

        bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport,
                         int mask, const Region &deviceRegion, LogicalOutput *screen) override;
                         bool isActive() const override;

    private Q_SLOTS:
        void slotMouseChanged(const QPointF &pos, const QPointF &old);

    private:
        void showCursor();
        void hideCursor();
        void refreshCursorHidingEffects();
        bool anotherEffectHidesCursor() const;
        GLTexture *ensureCursorTexture();
        void markCursorTextureDirty();

        std::unique_ptr<GLTexture> m_cursorTexture;
        bool m_cursorTextureDirty = false;
        bool m_isMouseHidden = false;

        // Logical bounding box of the cursor as it was last drawn by paintScreen().
        // slotMouseChanged() damages this footprint so partial repaints erase the
        // previous cursor and never leave stale inverted pixels behind.
        QRect m_lastCursorRect;

        // Cached pointers to effects that also call effects->hideCursor() and
        // draw their own cursor representation. QPointer auto-nulls if the effect
        // is unloaded, so these are always safe to dereference.
        QPointer<Effect> m_shakeCursor;
        QPointer<Effect> m_hideCursor;
    };

} // namespace KWin
