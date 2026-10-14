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
#include "opengl/glutils.h"

#include <QPainter>

#include <algorithm>
#include <optional>

namespace KWin
{

/**
 * Fragment shader which replaces the legacy fixed-function GL_XOR logic op.
 *
 * It expects the cursor shape as a pure white premultiplied texture bound to
 * the default sampler ("color"/"alpha") and the current screen contents in
 * the "screen" sampler. The blending equation implemented here is
 *     result = screen * (1 - a) + (color - screen * color)
 * which for a white cursor (rgb == a) reduces to
 *     result = mix(screen, inv_srgb(screen), a)
 * Fully transparent pixels leave the screen untouched, fully opaque pixels
 * are inverted and partially covered pixels are blended accordingly. Because
 * the operation is its own inverse, painting the same cursor again restores
 * the original content - just like XOR did, but without requiring
 * GL_COLOR_LOGIC_OP (which is deprecated on desktop GL and unavailable in
 * GLES at all) and while keeping the result correct in linear/HDR spaces.
 */
class XorShaderFragment : public ShaderFragment
{
public:
    XorShaderFragment()
        : ShaderFragment(RenderPass::Final)
    {
    }
    std::optional<Introspection> introspect() const override
    {
        Introspection ret;
        ret.textures.append({QStringLiteral("screen"), false});
        return ret;
    }
    QString source() const override
    {
        return u""_s
            R"(
            uniform highp vec4 invSdrWhite;
            void main()
            {
                highp vec4 screen = texture(screenSampler, vTexcoord);
                // per-channel inversion, clamped to [0, 1]
                highp vec4 inverted = abs(screen - vec4(invSdrWhite));
                gl_FragColor = color * (1.0 - alpha) + (inverted - screen * inverted);
            })";
    }
};

XorCursorEffect::XorCursorEffect()
{
    hideCursor();
}

XorCursorEffect::~XorCursorEffect()
{
    showCursor();
}

GLTexture *XorCursorEffect::ensureCursorTexture()
{
    if (!m_cursorTexture || m_cursorTextureDirty) {
        m_cursorTexture.reset();
        m_cursorTextureDirty = false;
        const auto cursor = effects->cursorImage();
        if (!cursor.image().isNull()) {
            m_cursorTexture = GLTexture::upload(cursor.image());
            if (!m_cursorTexture) {
                return nullptr;
            }
            m_cursorTexture->setWrapMode(GL_CLAMP_TO_EDGE);
        }
    }
    return m_cursorTexture.get();
}

void XorCursorEffect::markCursorTextureDirty()
{
    m_cursorTextureDirty = true;
}

void XorCursorEffect::updateXorMaskTexture()
{
    // Build a pure white premultiplied mask from the cursor's alpha channel
    // (RGB = 255 * a, A = a). The XOR fragment shader only needs this alpha
    // coverage value to compute the inversion; anti-aliased edge pixels blend
    // linearly between the untouched screen and the inverted screen.
    const QImage image = effects->cursorImage().image();
    if (image.isNull()) {
        m_xorMaskTexture.reset();
        return;
    }
    QImage mask(image.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(qRgba(255, 255, 255, 0));
    QPainter painter(&mask);
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.drawImage(0, 0, image);
    painter.end();
    m_xorMaskTexture = GLTexture::upload(mask);
    if (m_xorMaskTexture) {
        m_xorMaskTexture->setWrapMode(GL_CLAMP_TO_EDGE);
    }
}

void XorCursorEffect::showCursor()
{
    if (m_isMouseHidden) {
        disconnect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
        disconnect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
        // the shader-inverted pixels are still on screen; repaint the last
        // painted cursor area so that the original content is restored before
        // KWin starts drawing its own cursor again
        if (!m_cursorPaintRect.isEmpty()) {
            const QRect r = m_cursorPaintRect.toAlignedRect();
            effects->addRepaint(r.x(), r.y(), r.width(), r.height());
        }
        // show the previously hidden mouse-pointer and free the loaded texture/picture.
        effects->showCursor();
        m_cursorTexture.reset();
        m_xorMaskTexture.reset();
        // drop the cached shader too: its TextureUniform references the mask
        // texture we just released, so it must not outlive it
        m_xorShader.reset();
        m_screenUniform = nullptr;
        m_invSdrWhiteUniform = nullptr;
        m_cursorPaintRect = QRectF();
        m_isMouseHidden = false;
    }
}

void XorCursorEffect::hideCursor()
{
    if (!m_isMouseHidden) {
        // try to prepare the cursor image / mask texture and only hide the
        // hardware cursor if that succeeded
        GLTexture *texture = nullptr;
        if (effects->isOpenGLCompositing()) {
            texture = ensureCursorTexture();
            if (texture) {
                updateXorMaskTexture();
            }
        }
        if (texture && m_xorMaskTexture) {
            effects->hideCursor();
            connect(effects, &EffectsHandler::cursorShapeChanged, this, &XorCursorEffect::markCursorTextureDirty);
            connect(effects, &EffectsHandler::mouseChanged, this, &XorCursorEffect::slotMouseChanged);
            m_isMouseHidden = true;
            // force an initial paint of the cursor at its current position
            effects->addRepaintFull();
        }
    }
}

QRectF XorCursorEffect::cursorRectLogical() const
{
    const auto cursor = effects->cursorImage();
    const QSizeF size = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    return QRectF(p, size);
}

void XorCursorEffect::paintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const Region &deviceRegion, LogicalOutput *screen)
{
    effects->paintScreen(renderTarget, viewport, mask, deviceRegion, screen);
    if (!m_isMouseHidden) {
        return;
    }
    GLTexture *cursorTexture = ensureCursorTexture();
    if (!cursorTexture) {
        return;
    }

    const QSizeF cursorSize = cursorRectLogical().size();
    const QPointF p = cursorRectLogical().topLeft();
    const auto scale = viewport.scale();

    // remember where we painted so that the same area can be restored later
    m_cursorPaintRect = QRectF(p, cursorSize);

    // Only paint if the cursor actually intersects the region being repainted
    // in this pass. Painting outside of the dirty region would leave the
    // inverted state inconsistent (the previous inversion could not be
    // undone), which is exactly what this check prevents.
    const QRectF cursorDeviceRect(p.x() * scale, p.y() * scale, cursorSize.width() * scale, cursorSize.height() * scale);
    if (!deviceRegion.intersects(Rect(cursorDeviceRect.toAlignedRect()))) {
        return;
    }

    // Shader-based replacement for the legacy fixed-function GL_XOR logic
    // op; see XorShaderFragment for the math. The "color" of the drawn quad
    // comes from the white premultiplied mask texture, the "screen" sampler
    // holds the current framebuffer contents.
    if (m_cursorTextureDirty) {
        updateXorMaskTexture();
    }
    if (!m_xorMaskTexture) {
        return;
    }

    // The shader is cached by ShaderManager keyed on its fragment + uniform
    // setup, so build it once and reuse it across frames instead of pushing
    // a freshly-constructed shader (and duplicate uniforms) every paint pass.
    if (!m_xorShader) {
        m_xorShader = std::make_unique<Shader>();
        m_xorShader->add(std::make_unique<XorShaderFragment>());
        // the mask texture is only sampled through this uniform, so it must be
        // part of the shader's uniform set; the concrete texture is swapped in
        // per-draw via TextureUniform::rebind()
        m_screenUniform = new TextureUniform(GL_TEXTURE_2D,
                                             m_xorMaskTexture.get(),
                                             m_xorMaskTexture->size(),
                                             QStringLiteral("screen"));
        m_xorShader->addUniform(std::unique_ptr<TextureUniform>(m_screenUniform));
        m_invSdrWhiteUniform = new FloatUniform(QStringLiteral("invSdrWhite"), QOpenGLVersionProfile());
        m_xorShader->addUniform(std::unique_ptr<FloatUniform>(m_invSdrWhiteUniform));
    }

    const auto targetDesc = renderTarget.colorDescription();
    // The average (paper-white) intensity of the inversion, in the render
    // target's transfer space. For SDR/sRGB output this is simply 1.0; for
    // HDR targets we scale it down so the inverted cursor keeps a constant
    // perceived contrast instead of blindingly inverting towards peak white
    // (mirrors what KWin's own invert-screen effect does).
    float invSdrWhite = 1.0f;
    if (targetDesc.transfer != TransferFunction::sRGB && targetDesc.maxIntensity > 1.0f) {
        invSdrWhite = 80.0f / targetDesc.maxIntensity;
    }
    m_invSdrWhiteUniform->set(invSdrWhite);

    // pushShader() caches compiled GL programs by shader contents, so reusing
    // the same Shader object across frames only costs a hash lookup
    auto s = ShaderManager::instance()->pushShader(m_xorShader.get());
    // the mask texture may have been recreated (cursor shape change) since the
    // shader was built - point the sampler at the current texture before drawing
    m_screenUniform->rebind(m_xorMaskTexture.get(), m_xorMaskTexture->size());

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    s->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);
    cursorTexture->render(cursorSize * scale);
    ShaderManager::instance()->popShader();
}

bool XorCursorEffect::isActive() const
{
    return m_isMouseHidden;
}

void XorCursorEffect::slotMouseChanged(const QPointF &pos, const QPointF &old)
{
    if (pos == old) {
        return;
    }
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
