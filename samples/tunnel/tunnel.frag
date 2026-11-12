// Tunnel — raymarched glowing tunnel with coloured lights.
// Ported to omarwalls from hapatapa's Wallpaper Engine scene "I like tunnels"
// (shaders/main.frag). Self-contained main() style: shader-host supplies
// iResolution, iTime and the rest of the uniform block.

// Full-amplitude path used for the tunnel geometry.
vec3 path(float z) {
    return vec3(cos(z * 0.015) * 16.0 + cos(z * 0.006) * 64.0,
                cos(z * 0.011) * 24.0 + cos(z * 0.009) * 32.0,
                z);
}

// Balanced path used for the camera and look-ahead target.
vec3 camera_path(float z) {
    return vec3(cos(z * 0.015) * 11.2 + cos(z * 0.006) * 44.8,
                cos(z * 0.011) * 16.8 + cos(z * 0.009) * 22.4,
                z);
}

mat2 rot2(float a) {
    float c = cos(a);
    float s = sin(a);
    return mat2(c, -s, s, c);
}

float boxen(vec3 p) {
    vec3 pp = abs(fract(p / 40.0) * 40.0 - 20.0) - 2.0;
    return min(pp.x, min(pp.y, pp.z));
}

vec4 lights;

float map(vec3 p) {
    vec3 q = path(p.z);
    float g = q.y - p.y + 6.0;
    float m = boxen(p);
    vec2 p_local = p.xy - q.xy;

    // two strings of lights running along the tunnel
    vec2 s_z = sin(p.y / 12.0 + vec2(5.0, 0.0));
    float red = length(p_local - s_z.x * 12.0) - 1.0;
    float blue = length(p_local - s_z.y * 12.0) - 1.0;

    lights += vec4(20.0, 0.0, 15.0, 0.0) / (0.1 + abs(red) * 0.1);
    lights += vec4(0.0, 15.0, 20.0, 0.0) / (0.1 + abs(blue) * 0.1);

    vec3 p_dist = abs(p - q);
    float tex = (abs(sin(p_dist.x * 0.1)) + abs(sin(p_dist.y * 0.1))) * 0.05;
    float tun = min(64.0 - p_dist.x - p_dist.y + m, 32.0 - p_dist.y - m);

    return max(min(m, g), tun) - tex;
}

void main() {
    float i = 0.0;
    float s = 0.0;
    float d = 0.0;
    vec2 r = iResolution.xy;
    vec2 uv = (gl_FragCoord.xy - r.xy / 2.0) / r.y;
    uv.y -= 0.2;

    float T = sin(iTime * 0.6) * 64.0 + iTime * 80.0;

    vec4 o = vec4(0.0, 0.0, 0.0, 0.0);
    lights = vec4(0.0, 0.0, 0.0, 0.0);

    vec3 ro = camera_path(T);
    vec3 targ = camera_path(T + 60.0);

    vec3 Z_dir = normalize(targ - ro);
    vec3 X_dir = normalize(vec3(Z_dir.z, 0.0, -Z_dir.x));
    vec3 Y_dir = cross(Z_dir, X_dir);

    vec3 D = normalize(mat3(-X_dir, Y_dir, Z_dir)
        * vec3(rot2(sin(T * 0.005) * 0.4) * uv, 1.0));

    for (i = 0.0; i < 80.0; i = i + 1.0) {
        s = map(ro + D * d);
        d = d + s * 1.1;
        if (s < 0.1) break;
        if (d > 800.0) break;
    }

    o = lights * 0.0008;

    // soft per-channel tonemap
    o.x = o.x / (1.0 + o.x);
    o.y = o.y / (1.0 + o.y);
    o.z = o.z / (1.0 + o.z);

    o.x = pow(o.x, 1.0 / 1.2);
    o.y = pow(o.y, 1.0 / 1.2);
    o.z = pow(o.z, 1.0 / 1.2);

    fragColor = vec4(o.xyz, 1.0);
}
