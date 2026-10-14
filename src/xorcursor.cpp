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

#include <QRegion>

namespace KWin
{

/**
 * Fragment shader which replaces the legacy fixed-function GL_XOR logic op.
 *
 * It is generated through ShaderManager with the MapTexture trait, so the
 * cursor image itself is sampled from the default ("sampler") texture and the
 * current framebuffer contents are read back from an additional sampler
 * ("screenSampler") bound to texture unit 1.
 *
 * The math works in *straight* (un-premultiplied) color space, which makes it
 * independent of the cursor's actual colors - any standard cursor theme works,
 * not just white ones:
 *
 *   c  = unpremultiply(cursor.rgb / cursor.a)   (displayed cursor color)
 *   x  = |screen - W|                            (XOR-style inversion,
 *                                                 W = white reference level)
 *   out = mix(screen, x, a)                      (per-pixel coverage blend)
 *
 * Key properties:
 *  - The blend factor is the raw coverage alpha only; the cursor's RGB never
 *    enters the result except through the inversion-free identity path. This
 *    means the operation is exactly its own inverse when applied twice over
 *    the same area with the same alpha coverage - repainting the old bounding
 *    box restores the original screen content, just like XOR did.
 *  - Fully transparent pixels (a == 0) leave the screen untouched, fully
 *    opaque pixels invert it, anti-aliased edge pixels blend linearly.
 *  - No GL_COLOR_LOGIC_OP required, so this also works on OpenGL ES and
 *    modern core profiles.
 *
 * Note: generateCustomShader() prepends "#define TRAIT_*" lines before our
 * source, so the fragment shader must not carry its own #version directive
 * (it would no longer be the first line then).
 */
static const QByteArray s_xorFragmentSource = QByteArrayLiteral(R"(
uniform sampler2D sampler;
uniform sampler2D screenSampler;
uniform float invSdrWhite;

in vec2 texcoord0;

out vec4 fragColor;

void main(void)
{
    vec4 cursor = texture(sampler, texcoord0);
    vec4 screen = texture(screenSampler, texcoord0);
    float a = clamp(cursor.a, 0.0, 1.0);
    // XOR-style inversion of the screen around the white reference level
    vec3 inverted = abs(screen.rgb - vec3(invSdrWhite));
    // coverage-only blend: independent of the cursor's own colors, and
    // applying it twice restores the original screen content
    fragColor = vec4(mix(screen.rgb, inverted, a), screen.a);
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

    // (Re-)upload the cursor image itself whenever the shape changed. No
    // color preprocessing is done: the shader derives everything from the
    // raw alpha coverage, so any cursor theme (white, black, colored,
    // anti-aliased) behaves identically.
    if (!m_cursorTexture || m_cursorTextureDirty) {
        m_cursorTextureDirty = false;
        auto newTexture = GLTexture::upload(cursor.image());
        if (!newTexture) {
            return false;
        }
        newTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        m_cursorTexture = std::move(newTexture);
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
    // uniforms reference the cursor/screen textures and must not outlive them
    m_xorShader.reset();
    m_cursorTexture.reset();
    m_cursorTextureDirty = true;
}

void XorCursorEffect::markCursorTextureDirty()
{
    m_cursorTextureDirty = true;
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
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
        // try to prepare the cursor texture / shader and only hide the
        // hardware cursor if that succeeded
        if (effects->isOpenGLCompositing() && ensureGlResources()) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
            m_isMouseHidden = true;
            // force an initial paint of the cursor at its current position
            effects->addRepaintFull();
        }
    }
}

QRectF XorCursorEffect::cursorRectLogical() const
{
    const PlatformCursorImage cursor = effects->cursorImage();
    const double dpr = cursor.image().devicePixelRatio();
    const QSizeF size = QSizeF(cursor.image().size()) / dpr;
    const QPointF p = effects->cursorPos() - QPointF(cursor.hotSpot()) / dpr;
    return QRectF(p, size);
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    // snapshot where we last inverted *before* the base pass repaints those
    // pixels back to their original content
    const QRectF oldPaintRect = m_cursorPaintRect;

    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }
    if (!ensureGlResources()) {
        return;
    }

    const PlatformCursorImage cursor = effects->cursorImage();
    const double dpr = cursor.image().devicePixelRatio();
    const QSizeF sizeLogical = QSizeF(cursor.image().size()) / dpr;
    const QPointF topLeftLogical = effects->cursorPos() - QPointF(cursor.hotSpot()) / dpr;

    const double scale = viewport.scale();
    const QRectF cursorRectDevice(topLeftLogical.x() * scale,
                                  topLeftLogical.y() * scale,
                                  sizeLogical.width() * scale,
                                  sizeLogical.height() * scale);

    // The inversion is stateful: it must be applied exactly once over the
    // current bounding box. Only paint when this output's device region fully
    // contains that box; partial repaints (e.g. the cursor straddling an
    // output edge or a partial damage rect) are skipped so that no half
    // inversion can ever be baked into the framebuffer. The cursor simply
    // doesn't show for that frame on that output instead of corrupting pixels.
    if (!deviceRegion.rect().toQRect().contains(cursorRectDevice.toAlignedRect())) {
        m_cursorPaintRect = QRectF();
        return;
    }

    // If the previous inversion area was not restored by the base repaint
    // pass (it fell outside this pass' damage), skip this frame rather than
    // stacking a second inversion on top of a stale one. The next full
    // repaint will resynchronize both.
    if (!oldPaintRect.isEmpty()) {
        const QRectF oldDeviceRect(oldPaintRect.topLeft() * scale,
                                   oldPaintRect.size() * scale);
        if (!deviceRegion.rect().toQRect().contains(oldDeviceRect.toAlignedRect())) {
            m_cursorPaintRect = QRectF();
            return;
        }
    }

    // The white reference level for the inversion, in the render target's
    // transfer space. For SDR/sRGB output this is simply 1.0; for HDR targets
    // we scale it down so the inverted cursor keeps a constant perceived
    // contrast instead of blindingly inverting towards peak white.
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

    // texture unit 0: the cursor image (sampled by the base MapTexture
    // sampler; only its alpha channel is used as coverage);
    // texture unit 1: the framebuffer we are compositing into, read back
    // through screenSampler. Both use the very same texture coordinate
    // mapping (viewport projection applied to the cursor quad), so the
    // screen sample lines up pixel-exactly with the drawn fragment.
    glActiveTexture(GL_TEXTURE0);
    m_cursorTexture->bind();
    shader->setUniform("sampler", 0);

    glActiveTexture(GL_TEXTURE1);
    renderTarget.texture()->bind();
    shader->setUniform("screenSampler", 1);

    shader->setUniform("invSdrWhite", invSdrWhite);

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(topLeftLogical.x() * scale, topLeftLogical.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    // Blending must be disabled: the shader already computes the final pixel
    // value and writes it directly (its alpha passthrough keeps the result
    // opaque). Any residual blend state from the base pass would corrupt the
    // inversion and break the self-inverse property.
    const bool blendingWasEnabled = GLFramebuffer::isBlendingEnabled();
    glDisable(GL_BLEND);

    m_cursorTexture->render(sizeLogical * scale);

    if (blendingWasEnabled) {
        glEnable(GL_BLEND);
    }

    // restore the active texture unit the scene expects
    glActiveTexture(GL_TEXTURE0);
    ShaderManager::instance()->popShader();

    // remember the exact device-space area we inverted, so that restoring it
    // later can check containment against the dirty region without rounding
    // mismatches
    m_cursorPaintRect = QRectF(cursorRectDevice.x() / scale,
                               cursorRectDevice.y() / scale,
                               cursorRectDevice.width() / scale,
                               cursorRectDevice.height() / scale);
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
