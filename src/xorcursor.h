/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/colorspace.h"
#include "effect/effect.h"

namespace KWin
{

class GLFramebuffer;
class GLTexture;
class GLVertexBuffer;
class GLShader;

class XorCursorEffect : public Effect
{
    Q_OBJECT

public:
    XorCursorEffect();
    ~XorCursorEffect() override;

    bool paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    bool isActive() const override;

private Q_SLOTS:
    void slotMouseChanged(const QPointF &pos, const QPointF &old);

private:
    void showCursor();
    void hideCursor();
    GLTexture *ensureCursorTexture();
    void markCursorTextureDirty();
    // Global-scene-coordinate rect occupied by the cursor sprite when its
    // hotspot is at |hotspotPos| and the texture has |textureSize| device px.
    QRectF cursorRectAt(const QPointF &hotspotPos, const QSize &textureSize) const;
    // Requests a repaint of exactly |region| (global scene coords) on every
    // output it intersects. Used instead of full-screen repaints so that only
    // the pixels whose inverted XOR state actually changes get recomposited.
    void addRepaintForRegion(const Region &region);

    std::unique_ptr<GLTexture> m_cursorTexture;
    bool m_cursorTextureDirty = false;
    bool m_isMouseHidden = false;
    // Footprint (global scene coords) of the cursor pixels that are currently
    // XOR-inverted on screen — i.e. the sprite drawn during the last composited
    // frame. Null when nothing has been painted yet. Tracked so the next mouse
    // move can restore exactly those pixels.
    QRect m_paintedRect;
    // Accumulated set of regions that still must be refreshed before the next
    // frame is composited. Handles multiple mouse moves / shape changes per
    // frame correctly, and regions that span several outputs.
    Region m_pendingRegion;
};

} // namespace KWin
