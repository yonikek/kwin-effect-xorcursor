/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "xorcursor.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include "effect/effecthandler.h"
#include "opengl/glshader.h"
#include "opengl/glshadermanager.h"
#include "opengl/gltexture.h"

#include <epoxy/gl.h>

#include <QPainter>

namespace KWin
{

/**
 * Fragment shader which replaces the legacy fixed-function GL_XOR logic op.
 *
 * It is generated through ShaderManager with the MapTexture trait, so the
 * white premultiplied-alpha cursor mask is sampled from the default
 * ("sampler") texture unit and the current framebuffer contents are read back
 * from an additional sampler bound to texture unit 1. The blending equation
 * implemented here is
 *     result = screen * (1 - a) + (inv(screen) - screen * inv(screen))
 * which for a white mask (rgb == a) reduces to
 *     result = mix(screen, inverse(screen), a)
 * Fully transparent pixels leave the screen untouched, fully opaque pixels
 * are inverted, and partially covered (anti-aliased) edge pixels blend
 * linearly between the two. Because the operation is its own inverse,
 * painting the same rectangle again restores the original content - just like
 * XOR did, but without requiring GL_COLOR_LOGIC_OP (deprecated on desktop GL
 * and unavailable on GLES altogether).
 *
 * Note: generateCustomShader() prepends "#define TRAIT_*" lines before our
 * source, so the fragment shader must not carry its own #version directive
 * (it would no longer be the first line then). The base vertex/fragment
 * shaders that KWin generates for us use GLSL 1.40, so we target the same.
 */
static const QByteArray s_xorFragmentSource = QByteArrayLiteral(R"(
uniform sampler2D sampler;
uniform sampler2D screenSampler;
uniform float invSdrWhite;

in vec2 texcoord0;

out vec4 fragColor;

void main(void)
{
    vec4 mask = texture(sampler, texcoord0);
    vec4 screen = texture(screenSampler, texcoord0);
    // per-channel inversion around the target's SDR white point
    vec4 inverted = abs(screen - vec4(invSdrWhite));
    // self-inverse blend: untouched where transparent, inverted where opaque
    fragColor = screen * (1.0 - mask.a) + (inverted - screen * inverted);
}
)");

XorCursorEffect::XorCursorEffect()
{
    hideCursor();
}

XorCursorEffect::~XorCursorEffect()
{
    showCursor();
}

bool XorCursorEffect::ensureGlResources()
{
    const PlatformCursorImage cursor = effects->cursorImage();
    if (cursor.image().isNull()) {
        return false;
    }

    // (Re-)build the pure white premultiplied mask (rgb == a) whenever the
    // cursor shape changed. The XOR shader only needs this coverage value;
    // anti-aliased edge pixels blend linearly between the untouched screen
    // and the inverted screen.
    if (!m_maskTexture || m_maskSize != cursor.image().size() || m_maskHotSpot != cursor.hotSpot().toPoint()) {
        QImage mask(cursor.image().size(), QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        QPainter painter(&mask);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.drawImage(0, 0, cursor.image());
        painter.end();
        auto newMask = GLTexture::upload(mask);
        if (!newMask) {
            return false;
        }
        newMask->setWrapMode(GL_CLAMP_TO_EDGE);
        m_maskTexture = std::move(newMask);
        m_maskSize = cursor.image().size();
        m_maskHotSpot = cursor.hotSpot().toPoint();
    }

    // Build the shader exactly once; ShaderManager caches the compiled GL
    // program keyed on the shader object itself.
    if (!m_xorShader) {
        m_xorShader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), s_xorFragmentSource);
        if (!m_xorShader) {
            // the custom fragment shader could not be compiled; keep the
            // hardware cursor visible rather than hiding it without a
            // replacement
            return false;
        }
    }

    return true;
}

void XorCursorEffect::releaseGlResources()
{
    // drop the cached shader together with the textures: the shader's sampler
    // uniforms reference the mask/screen textures and must not outlive them
    m_xorShader.reset();
    m_maskTexture.reset();
    m_maskSize = QSize();
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
        // the shader-inverted pixels are still on screen; repaint the last
        // painted cursor area so that the original content is restored before
        // KWin starts drawing its own hardware cursor again
        if (!m_cursorPaintRect.isEmpty()) {
            const QRect r = m_cursorPaintRect.toAlignedRect();
            effects->addRepaint(r.x(), r.y(), r.width(), r.height());
        }
        // show the previously hidden mouse-pointer and free the GL resources
        effects->showCursor();
        releaseGlResources();
        m_cursorPaintRect = QRectF();
        m_isMouseHidden = false;
    }
}

void XorCursorEffect::hideCursor()
{
    if (!m_isMouseHidden) {
        // try to prepare the cursor mask texture / shader and only hide the
        // hardware cursor if that succeeded
        if (effects->isOpenGLCompositing() && ensureGlResources()) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
            // force an initial paint of the cursor at its current position
            effects->addRepaintFull();
        }
    }
}

QRectF XorCursorEffect::cursorRectLogical() const
{
    const PlatformCursorImage cursor = effects->cursorImage();
    const QSizeF size = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - QPointF(cursor.hotSpot().toPoint()) / cursor.image().devicePixelRatio();
    return QRectF(p, size);
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }
    if (!ensureGlResources()) {
        return;
    }

    const QRectF cursorRect = cursorRectLogical();
    const QSizeF cursorSize = cursorRect.size();
    const QPointF p = cursorRect.topLeft();

    // remember where we painted so that the same area can be restored later
    m_cursorPaintRect = cursorRect;

    // Only paint if the cursor actually intersects the region being repainted
    // in this pass. Painting outside of the dirty region would leave the
    // inverted state inconsistent (the previous inversion could not be
    // undone), which is exactly what this check prevents.
    const double scale = viewport.scale();
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
    if (!deviceRegion.intersects(Rect(cursorDeviceRect.toAlignedRect()))) {
        return;
    }

    // The average (paper-white) intensity of the inversion, in the render
    // target's transfer space. For SDR/sRGB output this is simply 1.0; for
    // HDR targets we scale it down so the inverted cursor keeps a constant
    // perceived contrast instead of blindingly inverting towards peak white.
    // ColorDescription::referenceLuminance()/minLuminance() are absolute
    // nits values (SDR reference defaults to 80 nits), while
    // TransferFunction::maxLuminance is relative-ish encoded value at 1.0,
    // so we normalize by the target's own min luminance before dividing.
    float invSdrWhite = 1.0f;
    const std::shared_ptr<ColorDescription> &desc = renderTarget.colorDescription();
    if (desc && desc->transferFunction().type != TransferFunction::sRGB) {
        const double maxLuminance = desc->transferFunction().maxLuminance;
        const double minLuminance = desc->minLuminance();
        if (maxLuminance > 1.0 && minLuminance > 0.0) {
            invSdrWhite = static_cast<float>(desc->referenceLuminance() / minLuminance / maxLuminance);
        }
    }

    GLShader *shader = m_xorShader.get();
    ShaderManager::instance()->pushShader(shader);

    // texture unit 0: the white cursor mask (sampled by the base MapTexture
    // sampler); texture unit 1: the framebuffer we are compositing into,
    // read back through screenSampler. Both use the very same texture
    // coordinate mapping (viewport projection applied to the cursor quad),
    // so the screen sample lines up pixel-exactly with the drawn fragment.
    glActiveTexture(GL_TEXTURE0);
    m_maskTexture->bind();
    shader->setUniform("sampler", 0);

    glActiveTexture(GL_TEXTURE1);
    renderTarget.texture()->bind();
    shader->setUniform("screenSampler", 1);

    shader->setUniform("invSdrWhite", invSdrWhite);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    m_maskTexture->render(cursorSize * scale);

    // restore the active texture unit the scene expects
    glActiveTexture(GL_TEXTURE0);
    ShaderManager::instance()->popShader();
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged()
{
    // Because the cursor is inverted in-place, both the area where the cursor
    // was previously painted and the area where it will be painted now have to
    // be repainted: the old one to restore the original pixels, the new one to
    // invert them. Repainting only these two rectangles instead of the whole
    // screen massively reduces the amount of work per frame.
    auto addLogicRectRepaint = [this](const QRectF &rect) {
        const QRect r = rect.toAlignedRect();
        effects->addRepaint(r.x(), r.y(), r.width(), r.height());
    };
    if (!m_cursorPaintRect.isEmpty()) {
        addLogicRectRepaint(m_cursorPaintRect);
    }
    addLogicRectRepaint(cursorRectLogical());
}

} // namespace KWin

#include "moc_xorcursor.cpp"
