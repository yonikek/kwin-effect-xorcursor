#ifndef KWIN_XORCURSOR_H
#define KWIN_XORCURSOR_H

#include <kwineffects.h>
#include <kwinglutils.h>

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QScopedPointer>
#include <QTimer>

#include <memory>

namespace KWin {

    class GLShader;
    class GLTexture;
    class GLRenderTarget;

    class XorCursorEffect : public Effect
    {
        Q_OBJECT
        Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled)

    public:
        XorCursorEffect();
        ~XorCursorEffect() override;

        void reconfigure(ReconfigureFlags flags) override;
        void prePaintScreen(ScreenPrePaintData &data) override;
        void paintScreen(const RenderTarget &renderTarget,
                         const RenderViewport &viewport,
                         int mask,
                         const QRegion &region,
                         ScreenPaintData &data) override;
                         void postPaintScreen() override;
                         bool isActive() const override;

                         static bool supported();

                         bool isEnabled() const;
                         void setEnabled(bool enabled);

    private Q_SLOTS:
        void slotMouseChanged(const QPointF &pos, const QPointF &oldpos,
                              Qt::MouseButtons buttons, Qt::MouseButtons oldbuttons,
                              Qt::KeyboardModifiers modifiers,
                              Qt::KeyboardModifiers oldmodifiers);
        void markCursorTextureDirty();
        void hideCursor();
        void showCursor();

    private:
        void loadShader();
        void updateCursorTexture();
        bool ensureRenderTarget(const QSize &size);
        void applyXorShader(const QRect &deviceRect,
                            const QRect &cursorRect,
                            const QPointF &cursorHotspot);

        bool m_enabled = false;
        bool m_isMouseHidden = false;

        std::unique_ptr<GLTexture> m_cursorTexture;
        std::unique_ptr<GLTexture> m_backgroundTexture;
        std::unique_ptr<GLRenderTarget> m_renderTarget;
        GLShader *m_xorShader = nullptr;

        QSize m_renderTargetSize;
        QRect m_lastCursorRect;          // logical coordinates
        qreal m_lastScale = 1.0;         // device pixel ratio of the screen
        QImage m_cursorImage;

        QTimer m_repaintTimer;
    };

} // namespace KWin

#endif // KWIN_XORCURSOR_H
