#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in flat uint fragMeshletId;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    uint debug_mode;
} pc;

layout(location = 0) out vec4 outColor;

const vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));

vec3 meshlet_color(uint id) {
    // Knuth multiplicative hash -> distinct hue per meshlet
    uint h = id * 2654435761u;
    float r = float((h >> 16) & 0xFFu) / 255.0;
    float g = float((h >>  8) & 0xFFu) / 255.0;
    float b = float( h        & 0xFFu) / 255.0;
    return mix(vec3(0.15), vec3(r, g, b), 0.9);
}

void main() {
    vec3 N = normalize(fragNormal);
    float NdotL = max(dot(N, lightDir), 0.0);
    if (!gl_FrontFacing) NdotL = max(dot(-N, lightDir), 0.0);

    if (pc.debug_mode == 1u) {
        vec3 base = meshlet_color(fragMeshletId);
        outColor = vec4(base * (0.4 + 0.6 * NdotL), 1.0);
    } else {
        vec3 color = vec3(0.3) + vec3(0.7) * NdotL;
        outColor = vec4(color, 1.0);
    }
}
