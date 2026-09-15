#version 140

uniform sampler2D uCursorTexture;
uniform sampler2D uBackgroundTexture;
uniform vec2      uCursorPosition;
uniform vec2      uTextureSize;

in vec2 vTexCoord;
out vec4 fragColor;

uint toByte(float v) {
	return uint(clamp(v, 0.0, 1.0) * 255.0 + 0.5);
}

float fromByte(uint v) {
	return float(v) / 255.0;
}

void main() {
	vec4 cursor = texture(uCursorTexture, vTexCoord);
	vec2 bgCoord = (gl_FragCoord.xy - uCursorPosition) / uTextureSize;
	vec4 background = texture(uBackgroundTexture, bgCoord);

	uint r = toByte(cursor.r) ^ toByte(background.r);
	uint g = toByte(cursor.g) ^ toByte(background.g);
	uint b = toByte(cursor.b) ^ toByte(background.b);

	fragColor = vec4(fromByte(r), fromByte(g), fromByte(b), cursor.a);
}
