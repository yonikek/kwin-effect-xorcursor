#pragma once
#include <QImage>
#include <QSizeF>
#include <memory>
#include "glshaders.h"
#include <epoxy/gl.h>
namespace KWin {
enum { GL_CLAMP_TO_EDGE };
class GLTexture {
public:
    static std::unique_ptr<GLTexture> upload(const QImage &) { return nullptr; }
    void setWrapMode(int) {}
    QSize size() const { return {}; }
    void render(const QSizeF &) {}
};
}
