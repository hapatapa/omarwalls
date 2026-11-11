// test plasma — shadertoy-style mainImage wallpaper
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    uv.x *= iResolution.x / iResolution.y;
    float t = iTime * 0.5;
    float c = sin(uv.x * 8.0 + t) * cos(uv.y * 6.0 - t * 1.3);
    c += 0.5 * sin(length(uv - vec2(0.5)) * 12.0 - iTime * 2.0);
    vec3 base = mix(u_theme[0].rgb, u_theme[1].rgb, 0.5 + 0.5 * sin(c));
    vec3 col = mix(u_theme[0].rgb, base, 0.5 + 0.5 * c);
    float vign = smoothstep(1.2, 0.2, length(uv - vec2(0.5)));
    col *= 0.6 + 0.4 * vign;
    col += u_theme[2].rgb * 0.15 * (0.5 + 0.5 * sin(iTime * 0.3));
    fragColor = vec4(col, 1.0);
}