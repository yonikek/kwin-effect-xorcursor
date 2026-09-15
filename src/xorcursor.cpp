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

    const auto cursor = effects->cursorImage();
    QSizeF cursorSize = QSizeF(cursor.image().size()) / cursor.image().devicePixelRatio();
    const QPointF p = effects->cursorPos() - cursor.hotSpot();
    m_lastCursorRect = QRectF(p, cursorSize).toAlignedRect();
    const auto scale = viewport.scale();

    QSize cursorDeviceSize = (cursorSize * scale).toSize();
    if (cursorDeviceSize.isEmpty()) {
        return;
    }

    QRectF cursorDeviceRect(p.x() * scale, p.y() * scale,
                            cursorSize.width() * scale, cursorSize.height() * scale);
    Region cursorRegion = Region(Rect(cursorDeviceRect.toAlignedRect()));
    effects->paintScreen(renderTarget, viewport, mask, cursorRegion, screen);

    GLTexture *screenTexture = renderTarget.texture();
    bool useShader = false;

    if (screenTexture) {
        ensureXorShader();
        if (m_xorShader) {
            // Reallocate only when the size changes; match the source format.
            if (!m_cursorBgTexture || m_cursorBgTexture->size() != cursorDeviceSize) {
                GLenum fmt = screenTexture->internalFormat();
                if (!fmt) {
                    fmt = GL_RGBA8;
                }
                m_cursorBgTexture = GLTexture::allocate(fmt, cursorDeviceSize);
                if (m_cursorBgTexture) {
                    m_cursorBgTexture->setFilter(GL_LINEAR);
                    m_cursorBgTexture->setWrapMode(GL_CLAMP_TO_EDGE);
                }
            }

            if (m_cursorBgTexture) {
                GLFramebuffer tempFbo(m_cursorBgTexture.get());
                if (tempFbo.valid()) {
                    // *** KEY FIX: source rect in LOGICAL coordinates ***
                    QRectF logicalRect(p, cursorSize);
                    Rect srcRect(logicalRect.toAlignedRect());
                    Rect dstRect(QRect(0, 0, cursorDeviceSize.width(), cursorDeviceSize.height()));

                    useShader = tempFbo.blitFromRenderTarget(renderTarget, viewport, srcRect, dstRect);
                }
            }
        }
    }

    GLShader *shader = nullptr;
    if (useShader) {
        shader = m_xorShader.get();
        ShaderManager::instance()->pushShader(shader);

        glActiveTexture(GL_TEXTURE1);
        m_cursorBgTexture->bind();
        shader->setUniform("bgSampler", 1);
        glActiveTexture(GL_TEXTURE0);
    } else {
        auto s = ShaderManager::instance()->pushShader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
        if (!s) {
            return;
        }
        shader = s;
        s->setColorspaceUniforms(ColorDescription::sRGB, renderTarget.colorDescription(), RenderingIntent::Perceptual);
        glEnable(GL_COLOR_LOGIC_OP);
        glLogicOp(GL_XOR);
    }

    QMatrix4x4 mvp = viewport.projectionMatrix();
    mvp.translate(p.x() * scale, p.y() * scale);
    shader->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, mvp);

    cursorTexture->render(cursorSize * scale);

    ShaderManager::instance()->popShader();

    if (useShader) {
        glActiveTexture(GL_TEXTURE1);
        m_cursorBgTexture->unbind();
        glActiveTexture(GL_TEXTURE0);
    } else {
        glDisable(GL_COLOR_LOGIC_OP);
    }
}
