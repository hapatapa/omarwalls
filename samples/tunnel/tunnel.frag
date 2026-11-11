// Tunnel — perspective tunnel with rotating rays, self-contained main() style
// (writes fragColor directly, uses the injected uniforms).
void main() {
    vec2 uv = gl_FragCoord.xy / iResolution.xy;
    vec2 p = uv * 2.0 - 1.0;
    p.x *= iResolution.x / iResolution.y;

    float t = iTime * 0.5;
    float ang = atan(p.y, p.x);
    float r = length(p);

    // tunnel rings scrolling toward viewer
    float ring = sin(r * 18.0 - iTime * 3.0) * 0.5 + 0.5;
    // rotating rays
    float ray = sin(ang * 6.0 + iTime * 1.2 + r * 4.0) * 0.5 + 0.5;

    vec3 col = mix(u_theme[0].rgb, u_theme[2].rgb, ray * 0.6);
    col = mix(col, u_theme[4].rgb, ring * 0.25);
    // depth fade
    col *= 1.0 - smoothstep(0.55, 1.1, r);
    col += u_theme[1].rgb * 0.06 * sin(ang * 3.0 + t);

    fragColor = vec4(col, 1.0);
}