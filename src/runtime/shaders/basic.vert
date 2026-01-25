#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Push constants for MVP matrix
layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

// Outputs to fragment shader
layout(location = 0) out vec3 fragNormal;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    fragNormal = inNormal;
}
