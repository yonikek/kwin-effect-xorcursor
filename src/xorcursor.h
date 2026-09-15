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
    bool ensureBackgroundBuffer(const QSize &size);
    GLShader *ensureInvertShader();

    std::unique_ptr<GLTexture> m_cursorTexture;
    std::unique_ptr<GLTexture> m_backgroundTexture;
    std::unique_ptr<GLFramebuffer> m_backgroundFramebuffer;
    std::unique_ptr<GLShader> m_invertShader;
    bool m_cursorTextureDirty = false;
    bool m_isMouseHidden = false;
    QRect m_lastCursorRect;
};

} // namespace KWin
