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

    void paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen) override;
    bool isActive() const override;

private Q_SLOTS:
    void slotMouseChanged(const QPointF &pos, const QPointF &old);

private:
    void showCursor();
    void hideCursor();
    GLTexture *ensureCursorTexture();
    void markCursorTextureDirty();

    /**
     * Returns the bounding rectangle (in logical coordinates) of the currently
     * painted cursor image, i.e. the area which has XOR-composited pixels that
     * must be XOR-painted again in order to restore the original content.
     */
    QRectF cursorRectLogical() const;

    std::unique_ptr<GLTexture> m_cursorTexture;
    bool m_cursorTextureDirty = false;
    bool m_isMouseHidden = false;
    /** Bounding rect of the last painted cursor shape, in logical coordinates. */
    QRectF m_cursorPaintRect;
};

} // namespace KWin
