/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/effect.h"

#include <QPoint>
#include <QPointF>
#include <QRectF>
#include <QSize>

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

private:
    void showCursor();
    void hideCursor();
    /**
     * Builds (or refreshes) the pure-white premultiplied alpha mask texture of
     * the current cursor shape and the shader that performs the XOR inversion
     * against the framebuffer contents. Returns false if the GL resources
     * could not be created.
     */
    bool ensureGlResources();
    void releaseGlResources();

    /**
     * Returns the bounding rectangle (in logical coordinates) of the currently
     * painted cursor image, i.e. the area which has XOR-composited pixels that
     * must be XOR-painted again in order to restore the original content.
     */
    QRectF cursorRectLogical() const;

    /** Cached pure-white premultiplied alpha mask of the cursor shape. */
    std::unique_ptr<GLTexture> m_maskTexture;
    /** Cached shader implementing the XOR inversion. */
    std::unique_ptr<GLShader> m_xorShader;
    /** Size (in device pixels) of the mask texture currently uploaded. */
    QSize m_maskSize;
    /** Cursor hotspot at the time the mask was last built. */
    QPoint m_maskHotSpot;
    bool m_isMouseHidden = false;
    /** Bounding rect of the last painted cursor shape, in logical coordinates. */
    QRectF m_cursorPaintRect;
};

} // namespace KWin
