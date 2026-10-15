#pragma once
#include <QMatrix4x4>
#include <QSizeF>
namespace KWin {
enum ShaderTrait { MapTexture = 1 };
class GLShader {
public:
    enum class Mat4Uniform { ModelViewProjectionMatrix };
    void setUniform(Mat4Uniform, const QMatrix4x4 &) {}
};
class ShaderManager {
public:
    static ShaderManager *instance() { static ShaderManager s; return &s; }
    struct Guard {};
    Guard pushShader(ShaderTrait) { return {}; }
    void popShader() {}
};
}
