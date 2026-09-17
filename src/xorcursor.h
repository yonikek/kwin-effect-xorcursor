/* SPDX-FileCopyrightText: 2025 Jin Liu
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include "effect/effect.h"

#include <QPointF>
#include <QRegion>
#include <QRect>
#include <QSize>
#include <QTimer>

#include <memory>

namespace KWin {

    class GLShader;
    class GLTexture;

    /**
     * Draws the cursor as an inverting (XOR with all-ones) mask over the
     * composited scene, reproducing the classic X11 "XorCursor" behaviour.
     *
     * The cursor image contributes only its alpha channel: opaque pixels invert
     * the background, transparent pixels leave it untouched. Works with any
     * cursor theme, including dark outlines and multi-colour cursors.
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
        void flushPendingRepaints();

    private:
        void tryAcquireHide();
        void releaseHide();
        bool isHiddenByOtherEffect();

        GLTexture *cursorTexture();
        void invalidateCursorTexture();

        void ensureBackgroundTexture(const QSize &size);

        // The logical rect the cursor currently occupies (pos - hotspot, size).
        QRect cursorLogicalRect() const;

        // Queue `damage` and arrange for it to be flushed once per event-loop
        // iteration, coalescing all mouse-move signals that arrive between
        // frames into a single addRepaint() call.
        void queueDamage(const QRegion &damage);

        std::unique_ptr<GLTexture> m_cursorTexture;
        bool m_cursorTextureDirty = false;

        std::unique_ptr<GLTexture> m_backgroundTexture;
        QSize m_backgroundTextureSize;

        // Cached shader and its GLSL flavour. Both are populated on the first
        // paintScreen() call, when the GL context is guaranteed to be current.
        std::shared_ptr<GLShader> m_shader;
        bool m_useModernGlsl = false;

        bool m_hideAcquired = false;
        QRect m_lastCursorRect;

        // Batched repaint state.
        QTimer m_repaintTimer;
        QRegion m_pendingDamage;
        bool m_repaintScheduled = false;
    };

} // namespace KWin
