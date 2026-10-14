/*
    SPDX-FileCopyrightText: 2025 Jin Liu <m.liu.jin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later

    Core-profile / GLSL 140+ variant of the cursor invert shader (KWin's
    ShaderManager resolves ":/effects/cursor_invert/shaders/cursor_invert.frag"
    to this file by appending "_core" when a core context is active, exactly
    like the built-in invert effect ships invert.frag / invert_core.frag).

    Inverts the screen content underneath the cursor, in the style of
    Plasma's "Invert screen color" accessibility option: the composited
    backdrop is converted to linear nits, inverted with `1.0 - rgb` in
    gamma 2.2 space (to preserve perceptual contrast, like invert.frag
    does) and re-encoded for the render target.

    Two adaptations are required to use this idea as a cursor shader:

    * The composited screen content is read back through a grabber FBO that
      was blitted from the render target before this shader runs. Because
      the grab is only valid inside the current paint pass' dirty region,
      the backdrop is sampled relative to gl_FragCoord instead of the quad
      texture coordinates.

    * A cursor image is not opaque: only pixels covered by the shape should
      fully invert the background, anti-aliased edge pixels should invert
      partially and fully transparent pixels must leave the background
      untouched. That is achieved by mixing between the original backdrop
      and the inverted backdrop using the cursor alpha as coverage factor.

    Because the inversion is computed entirely in this shader and masked by
    the cursor's alpha channel, no fixed-function GL_COLOR_LOGIC_OP is used
    and any cursor colour or theme works: the RGB values of the cursor
    image are never consumed at all.
*/

#include "colormanagement.glsl"

// unit 0: background texture (the freshly grabbed, composited screen content)
uniform sampler2D backgroundSampler;
// unit 1: cursor shape texture (only its alpha channel is used)
uniform sampler2D cursorSampler;

in vec2 texcoord0;

out vec4 fragColor;

void main()
{
    // Sample the background at exactly this fragment's device position.
    // The vertex shader maps the cursor quad one-to-one onto the viewport,
    // hence gl_FragCoord already points at the right pixel of the full-size
    // background texture bound to backgroundSampler.
    const vec2 bgSize = vec2(textureSize(backgroundSampler, 0));
    vec4 backdrop = texture(backgroundSampler, gl_FragCoord.xy / bgSize);

    // Invert the backdrop like Plasma's invert effect does, but in linear
    // light so the operation stays correct on wide-gamut / HDR outputs.
    vec4 nits = sourceEncodingToNitsInDestinationColorspace(backdrop);
    // to preserve perceptual contrast, apply the inversion in gamma 2.2 space
    vec4 gammaSpace = nitsToEncoding(nits, gamma22_EOTF, 0.0, destinationReferenceLuminance);
    gammaSpace.rgb /= max(0.001, gammaSpace.a);
    gammaSpace.rgb = vec3(1.0) - gammaSpace.rgb;
    vec4 inverted = encodingToNits(gammaSpace, gamma22_EOTF, 0.0, destinationReferenceLuminance);
    inverted *= backdrop; // keep the framebuffer's own alpha untouched
    vec4 invertedBackdrop = nitsToDestinationEncoding(inverted);

    // Use the cursor's alpha as a coverage mask: opaque pixels show the
    // fully inverted backdrop, translucent edge pixels blend it with the
    // untouched backdrop, transparent pixels leave the backdrop alone.
    // KWin uploads cursor images without premultiplying them, so the raw
    // alpha channel is the coverage value directly.
    const float coverage = texture(cursorSampler, texcoord0).a;

    fragColor = mix(backdrop, invertedBackdrop, coverage);
}
