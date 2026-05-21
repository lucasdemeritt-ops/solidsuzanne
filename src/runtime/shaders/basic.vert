#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in uint inMeshletId;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
    uint debug_mode;
} pc;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out flat uint fragMeshletId;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragNormal = mat3(pc.model) * inNormal;
    fragMeshletId = inMeshletId;
}
