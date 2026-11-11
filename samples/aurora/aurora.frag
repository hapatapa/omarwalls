// Aurora — shadertoy-style, animated northern-lights curtain.
// Uses only Shadertoy uniforms so it works in the Vulkan shader-host.
// iResolution, iTime, iFrame, iMouse, iDate + u_theme[16] are provided.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float t = iTime * 0.4;

    // aurora curtains: layered sine noise stretched vertically
    vec3 col = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float f = float(i) * 0.7;
        float band = sin(uv.x * 2.1 + f + t * 0.6)
                   + cos(uv.x * 3.7 - f * 1.3 + t * 0.4)
                   + sin(uv.x * 1.3 + f * 2.1 - t * 0.8);
        band = band / 3.0;
        float curtain = exp(-pow(abs(uv.y + 0.15 + band * 0.28), 2.0) * 6.0);
        vec3 pal = mix(u_theme[2].rgb, u_theme[4].rgb, float(i) / 3.0);
        col += curtain * pal * (0.25 + 0.35 * sin(t * 0.7 + f * 2.0));
    }

    // stars
    vec2 sp = uv * vec2(40.0, 70.0);
    vec2 g = floor(sp);
    if (fract(sin(dot(g, vec2(12.9898, 78.233))) * 43758.5453) > 0.985) {
        float tw = 0.5 + 0.5 * sin(t * 3.0 + g.x * 7.0);
        col += 0.6 * tw * u_theme[1].rgb;
    }

    // horizon glow + vignette
    col += u_theme[2].rgb * 0.12 * exp(uv.y * 4.0);
    col *= smoothstep(-0.6, 1.0, uv.y) * 0.8;
    col *= 1.0 - 0.35 * length(uv) * length(uv);
    col = mix(col, u_theme[0].rgb, 0.25);

    fragColor = vec4(col, 1.0);
}