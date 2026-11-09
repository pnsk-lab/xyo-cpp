// RCAS operates in premultiplied space, then restores straight alpha
// for SDL's normal alpha-compositing path.
uniform sampler2D uSource;
uniform ivec2 uOutputSize;
uniform uvec4 uCon;
layout(location = 0) out vec4 outColor;

AF4 FsrRcasLoadF(ASU2 p) {
    return texelFetch(uSource, clamp(p, ivec2(0), uOutputSize - 1), 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

void main() {
    ivec2 pos = ivec2(gl_FragCoord.xy);
    float r, g, b;
    FsrRcasF(r, g, b, uvec2(pos), uCon);
    float alpha = clamp(texelFetch(uSource, pos, 0).a, 0.0, 1.0);
    vec3 rgb = alpha > (1.0 / 255.0)
        ? clamp(vec3(r, g, b) / alpha, vec3(0.0), vec3(1.0))
        : vec3(0.0);
    outColor = vec4(rgb, alpha);
}
