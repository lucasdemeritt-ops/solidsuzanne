#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Push constants for MVP and Model matrices
layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;  // For normal transformation
} pc;

// Outputs to fragment shader
layout(location = 0) out vec3 fragNormal;

void main() {
    gl_Position = pc.mvp * vec4(inPosition, 1.0);

    // Transform normal by model matrix (upper 3x3)
    // For correct normal transformation with non-uniform scaling,
    // we'd need inverse transpose, but this works for uniform scale
    mat3 normalMatrix = mat3(pc.model);
    fragNormal = normalMatrix * inNormal;
}
