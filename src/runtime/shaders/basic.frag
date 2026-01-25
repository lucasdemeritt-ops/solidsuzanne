#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragNormal;

// Output color
layout(location = 0) out vec4 outColor;

// Simple directional light
const vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));

void main() {
    vec3 N = normalize(fragNormal);

    // Simple Lambert diffuse with ambient
    float NdotL = max(dot(N, lightDir), 0.0);

    // Two-sided lighting
    if (!gl_FrontFacing) {
        NdotL = max(dot(-N, lightDir), 0.0);
    }

    vec3 color = vec3(0.3) + vec3(0.7) * NdotL;

    outColor = vec4(color, 1.0);
}
