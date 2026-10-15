#pragma once
#include <QObject>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <vector>
#include <memory>
#include "region.h"
#include "cursor.h"

namespace KWin {

class RenderTarget {};
class RenderViewport {};
class LogicalOutput : public QObject {
public:
    QRect globalGeometry() const { return {}; }
    QSize logicalSize() const { return {}; }
    void addRepaint(const Region &) {}
};

class Effect : public QObject {
public:
    virtual ~Effect() = default;
    virtual bool paintScreen(const RenderTarget &, const RenderViewport &, int, const Region &, LogicalOutput *) { return true; }
    virtual bool isActive() const { return false; }
};

class EffectsHandler : public QObject {
    Q_OBJECT
public:
    CursorImage cursorImage() const { return {}; }
    QPointF cursorPos() const { return {}; }
    bool isOpenGLCompositing() const { return true; }
    void hideCursor() {}
    void showCursor() {}
signals:
    void cursorShapeChanged();
    void mouseChanged(const QPointF &pos, const QPointF &old);
};
inline EffectsHandler *effects = nullptr;

class WorkspaceStub {
public:
    std::vector<LogicalOutput*> logicalOutputs() { return {}; }
};
inline WorkspaceStub *workspace() { static WorkspaceStub w; return &w; }

} // namespace KWin
using KWin::LogicalOutput;
using KWin::workspace;
using KWin::effects;
