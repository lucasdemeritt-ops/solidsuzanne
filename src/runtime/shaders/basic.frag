#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec2 fragUV;
layout(location = 2) in vec3 fragWorldPos;

// Output color
layout(location = 0) out vec4 outColor;

// Simple directional light
const vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
const vec3 lightColor = vec3(1.0, 0.98, 0.95);
const vec3 ambientColor = vec3(0.1, 0.12, 0.15);

// Base material color
const vec3 baseColor = vec3(0.8, 0.8, 0.8);

void main() {
    vec3 N = normalize(fragNormal);

    // Simple Lambert diffuse
    float NdotL = max(dot(N, lightDir), 0.0);

    // Two-sided lighting
    if (!gl_FrontFacing) {
        N = -N;
        NdotL = max(dot(N, lightDir), 0.0);
    }

    vec3 diffuse = baseColor * lightColor * NdotL;
    vec3 ambient = baseColor * ambientColor;

    vec3 color = ambient + diffuse;

    // Simple tone mapping
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0/2.2));

    outColor = vec4(color, 1.0);
}
