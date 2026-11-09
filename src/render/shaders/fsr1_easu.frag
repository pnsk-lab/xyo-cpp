// Appended after GPUOpen's ffx_a.h and ffx_fsr1.h.
// FSR EASU input is premultiplied to avoid transparent-edge color bleeding.
uniform sampler2D uSource;
uniform ivec2 uInputSize;
uniform ivec2 uOutputSize;
uniform int uSourcePremult;
uniform int uStraightOutput;
uniform uvec4 uCon0;
uniform uvec4 uCon1;
uniform uvec4 uCon2;
uniform uvec4 uCon3;
layout(location = 0) out vec4 outColor;

AF4 FsrEasuRF(AF2 p) {
    vec4 v = textureGather(uSource, p, 0);
    return uSourcePremult != 0 ? v : v * textureGather(uSource, p, 3);
}
AF4 FsrEasuGF(AF2 p) {
    vec4 v = textureGather(uSource, p, 1);
    return uSourcePremult != 0 ? v : v * textureGather(uSource, p, 3);
}
AF4 FsrEasuBF(AF2 p) {
    vec4 v = textureGather(uSource, p, 2);
    return uSourcePremult != 0 ? v : v * textureGather(uSource, p, 3);
}

void main() {
    uvec2 pos = uvec2(ivec2(gl_FragCoord.xy));
    vec3 rgb;
    FsrEasuF(rgb, pos, uCon0, uCon1, uCon2, uCon3);
    vec2 uv = gl_FragCoord.xy / vec2(uOutputSize);
    // Alpha is reconstruction-only: sprites/pen logic keep their original
    // 480x360 pixels. This filter never affects hit testing or pen storage.
    float alpha = clamp(texture(uSource, uv).a, 0.0, 1.0);
    rgb = clamp(rgb, vec3(0.0), vec3(alpha));
    if (uStraightOutput != 0) {
        rgb = alpha > (1.0 / 255.0)
            ? clamp(rgb / alpha, vec3(0.0), vec3(1.0))
            : vec3(0.0);
    }
    outColor = vec4(rgb, alpha);
}
