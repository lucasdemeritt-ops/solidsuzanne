#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// Push constants for transforms
layout(push_constant) uniform PushConstants {
    mat4 viewProjection;
    mat4 model;
} pc;

// Outputs to fragment shader
layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec2 fragUV;
layout(location = 2) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = pc.model * vec4(inPosition, 1.0);
    gl_Position = pc.viewProjection * worldPos;

    // Transform normal to world space (assuming uniform scale)
    fragNormal = mat3(pc.model) * inNormal;
    fragUV = inUV;
    fragWorldPos = worldPos.xyz;
}
