/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"

#include <QPointF>
#include <QRectF>

#include <memory>

namespace KWin
{

class GLShader;
class GLTexture;

class XorCursorEffect : public Effect
{
    Q_OBJECT

public:
    XorCursorEffect();
    ~XorCursorEffect() override;

    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    bool isActive() const override;

private Q_SLOTS:
    void slotMouseChanged();
    void markCursorTextureDirty();

private:
    void showCursor();
    void hideCursor();
    /**
     * Builds (or refreshes) the texture of the current cursor image and the
     * shader that performs the XOR inversion against the framebuffer
     * contents. Returns false if the GL resources could not be created.
     */
    bool ensureGlResources();
    void releaseGlResources();

    /**
     * Returns the bounding rectangle (in logical coordinates) of the currently
     * painted cursor image, i.e. the area which has XOR-composited pixels that
     * must be XOR-painted again in order to restore the original content.
     */
    QRectF cursorRectLogical() const;

    /** Cached texture of the current cursor image (uploaded on shape change). */
    std::unique_ptr<GLTexture> m_cursorTexture;
    /** Cached shader implementing the XOR inversion. */
    std::unique_ptr<GLShader> m_xorShader;
    /** Set when the cursor shape changed and the texture needs re-uploading. */
    bool m_cursorTextureDirty = true;
    bool m_isMouseHidden = false;
    /** Bounding rect of the last painted cursor shape, in logical coordinates. */
    QRectF m_cursorPaintRect;
};

} // namespace KWin
