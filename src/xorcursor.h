#ifndef XORCURSOR_H
#define XORCURSOR_H

#include <kwin/kwineffects.h>
#include <kwin/kwinglutils.h>
#include <QScopedPointer>
#include <QRect>

class XorCursorEffect : public KWin::Effect
{
    Q_OBJECT
public:
    XorCursorEffect();
    ~XorCursorEffect() override;

    void prePaintScreen(KWin::ScreenPrePaintData &data, int time) override;
    void paintScreen(int mask, const QRegion &region, KWin::ScreenPaintData &data) override;
    bool isActive() const override;

private Q_SLOTS:
    void slotCursorShapeChanged();
    void slotCursorPosChanged();

private:
    void updateTexture();
    void initShader();

    QScopedPointer<KWin::GLTexture> m_cursorTexture;
    QScopedPointer<KWin::GLShader> m_shader;
    bool m_textureDirty;
    QRect m_lastCursorRect;
};

#endif // XORCURSOR_H
