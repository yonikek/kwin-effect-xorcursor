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
        void ensureXorShader(); // New method

        std::unique_ptr<GLTexture> m_cursorTexture;
        std::unique_ptr<GLShader> m_xorShader; // New member
        bool m_cursorTextureDirty = false;
        bool m_isMouseHidden = false;
        QRect m_lastCursorRect;
    };

} // namespace KWin
