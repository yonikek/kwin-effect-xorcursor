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
    // Global-scene-coordinate bounding box occupied by the cursor sprite when
    // its hotspot is at |hotspotPos|. Rounded outwards so it always fully
    // covers the drawn quad.
    QRect spriteBoundingRectAt(const QPointF &hotspotPos) const;
    // Requests a repaint of exactly |region| (global scene coords) on every
    // output it intersects. Used instead of full-screen repaints so that only
    // the pixels whose inverted XOR state actually changes get recomposited.
    void addRepaintForRegion(const Region &region);

    std::unique_ptr<GLTexture> m_cursorTexture;
    bool m_cursorTextureDirty = false;
    bool m_isMouseHidden = false;
    // Footprint (global scene coords) of the sprite drawn during the last
    // composited frame — used to compute the symmetric difference on mouse
    // moves so only pixels whose visual state actually changes get repainted.
    QRect m_paintedRect;
    // Exact set of pixels (global scene coords) that are currently XOR-inverted
    // on screen. Tracked per frame so damage caused by other windows, cursor
    // shape changes or multi-output repaints never leaves ghost images: any
    // inverted pixel that gets recomposed is re-inverted, and anything still
    // pending keeps requesting repaints until it has been refreshed.
    Region m_state;
};

} // namespace KWin
