// VGEO Camera
// Orbit camera with frustum for culling

#include "camera.h"
#include <cmath>
#include <cstring>

namespace vgeo {

// Helper math functions
namespace {

constexpr float PI = 3.14159265358979323846f;

inline float radians(float degrees) {
    return degrees * PI / 180.0f;
}

inline void normalize3(float* v) {
    float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

inline void cross3(float* out, const float* a, const float* b) {
    out[0] = a[1]*b[2] - a[2]*b[1];
    out[1] = a[2]*b[0] - a[0]*b[2];
    out[2] = a[0]*b[1] - a[1]*b[0];
}

inline float dot3(const float* a, const float* b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

inline void sub3(float* out, const float* a, const float* b) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

inline void add3(float* out, const float* a, const float* b) {
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
}

inline void scale3(float* out, const float* v, float s) {
    out[0] = v[0] * s;
    out[1] = v[1] * s;
    out[2] = v[2] * s;
}

// Matrix multiplication (column-major): out = a * b
inline void mul_mat4(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            tmp[col*4 + row] = 0.0f;
            for (int k = 0; k < 4; k++) {
                tmp[col*4 + row] += a[k*4 + row] * b[col*4 + k];
            }
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

// Identity matrix
inline void identity_mat4(float* m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Look-at matrix (column-major)
inline void look_at(float* out, const float* eye, const float* target, const float* up) {
    float f[3], s[3], u[3];

    // Forward (z-axis, pointing from target to eye for right-handed)
    sub3(f, eye, target);
    normalize3(f);

    // Right (x-axis)
    float up_normalized[3] = {up[0], up[1], up[2]};
    normalize3(up_normalized);
    cross3(s, up_normalized, f);
    normalize3(s);

    // Up (y-axis)
    cross3(u, f, s);

    // Column-major matrix
    out[0] = s[0];  out[4] = s[1];  out[8]  = s[2];  out[12] = -dot3(s, eye);
    out[1] = u[0];  out[5] = u[1];  out[9]  = u[2];  out[13] = -dot3(u, eye);
    out[2] = f[0];  out[6] = f[1];  out[10] = f[2];  out[14] = -dot3(f, eye);
    out[3] = 0.0f;  out[7] = 0.0f;  out[11] = 0.0f;  out[15] = 1.0f;
}

// Perspective matrix (column-major, Vulkan NDC: y-down, z [0,1])
// (parameters named z_near/z_far because Windows headers #define near/far)
inline void perspective_vulkan(float* out, float fov_y_rad, float aspect, float z_near, float z_far) {
    float tan_half_fov = std::tan(fov_y_rad / 2.0f);

    std::memset(out, 0, sizeof(float) * 16);

    out[0]  = 1.0f / (aspect * tan_half_fov);
    out[5]  = -1.0f / tan_half_fov;  // Negative for Vulkan y-down
    out[10] = z_far / (z_near - z_far);
    out[11] = -1.0f;
    out[14] = (z_near * z_far) / (z_near - z_far);
}

} // anonymous namespace

void Camera::update() {
    // Compute view matrix
    look_at(view, position, target, up);

    // Compute projection matrix
    perspective_vulkan(projection, radians(fov), aspect, near_plane, far_plane);

    // Compute combined view-projection
    mul_mat4(view_projection, projection, view);

    // Extract frustum planes from view-projection matrix (Gribb-Hartmann method)
    // Each plane: ax + by + cz + d = 0
    // Stored as [a, b, c, d] where (a,b,c) is normal pointing inward

    const float* m = view_projection;

    // Left plane: row3 + row0
    frustum_planes[0][0] = m[3]  + m[0];
    frustum_planes[0][1] = m[7]  + m[4];
    frustum_planes[0][2] = m[11] + m[8];
    frustum_planes[0][3] = m[15] + m[12];

    // Right plane: row3 - row0
    frustum_planes[1][0] = m[3]  - m[0];
    frustum_planes[1][1] = m[7]  - m[4];
    frustum_planes[1][2] = m[11] - m[8];
    frustum_planes[1][3] = m[15] - m[12];

    // Bottom plane: row3 + row1
    frustum_planes[2][0] = m[3]  + m[1];
    frustum_planes[2][1] = m[7]  + m[5];
    frustum_planes[2][2] = m[11] + m[9];
    frustum_planes[2][3] = m[15] + m[13];

    // Top plane: row3 - row1
    frustum_planes[3][0] = m[3]  - m[1];
    frustum_planes[3][1] = m[7]  - m[5];
    frustum_planes[3][2] = m[11] - m[9];
    frustum_planes[3][3] = m[15] - m[13];

    // Near plane: row2 alone (Vulkan clip space has z in [0,1], so the
    // near-side constraint is z >= 0, not the GL-style z >= -w)
    frustum_planes[4][0] = m[2];
    frustum_planes[4][1] = m[6];
    frustum_planes[4][2] = m[10];
    frustum_planes[4][3] = m[14];

    // Far plane: row3 - row2
    frustum_planes[5][0] = m[3]  - m[2];
    frustum_planes[5][1] = m[7]  - m[6];
    frustum_planes[5][2] = m[11] - m[10];
    frustum_planes[5][3] = m[15] - m[14];

    // Normalize planes
    for (int i = 0; i < 6; i++) {
        float len = std::sqrt(
            frustum_planes[i][0] * frustum_planes[i][0] +
            frustum_planes[i][1] * frustum_planes[i][1] +
            frustum_planes[i][2] * frustum_planes[i][2]
        );
        if (len > 1e-6f) {
            frustum_planes[i][0] /= len;
            frustum_planes[i][1] /= len;
            frustum_planes[i][2] /= len;
            frustum_planes[i][3] /= len;
        }
    }
}

void Camera::orbit(float delta_yaw, float delta_pitch) {
    // Compute current direction from target to position
    float dir[3];
    sub3(dir, position, target);
    float radius = std::sqrt(dot3(dir, dir));

    if (radius < 1e-6f) return;

    // Normalize direction
    dir[0] /= radius;
    dir[1] /= radius;
    dir[2] /= radius;

    // Convert to spherical coordinates
    float theta = std::atan2(dir[0], dir[2]);  // yaw (around Y axis)
    float phi = std::asin(std::fmax(-1.0f, std::fmin(1.0f, dir[1])));  // pitch

    // Apply deltas
    theta += delta_yaw;
    phi += delta_pitch;

    // Clamp pitch to avoid gimbal lock
    constexpr float max_pitch = PI / 2.0f - 0.01f;
    phi = std::fmax(-max_pitch, std::fmin(max_pitch, phi));

    // Convert back to Cartesian
    float cos_phi = std::cos(phi);
    position[0] = target[0] + radius * cos_phi * std::sin(theta);
    position[1] = target[1] + radius * std::sin(phi);
    position[2] = target[2] + radius * cos_phi * std::cos(theta);
}

void Camera::pan(float delta_x, float delta_y) {
    // Compute right and up vectors from view matrix
    float right[3] = {view[0], view[4], view[8]};
    float cam_up[3] = {view[1], view[5], view[9]};

    // Scale by distance for consistent feel
    float dir[3];
    sub3(dir, position, target);
    float dist = std::sqrt(dot3(dir, dir));
    float scale = dist * 0.002f;  // Adjust sensitivity

    // Apply panning
    float offset[3];
    scale3(offset, right, -delta_x * scale);
    add3(position, position, offset);
    add3(target, target, offset);

    scale3(offset, cam_up, delta_y * scale);
    add3(position, position, offset);
    add3(target, target, offset);
}

void Camera::zoom(float delta) {
    // Move position toward/away from target
    float dir[3];
    sub3(dir, position, target);
    float dist = std::sqrt(dot3(dir, dir));

    // Zoom factor (multiplicative)
    float factor = 1.0f - delta * 0.1f;
    factor = std::fmax(0.1f, std::fmin(10.0f, factor));

    float new_dist = dist * factor;
    new_dist = std::fmax(0.1f, new_dist);  // Minimum distance

    normalize3(dir);
    scale3(dir, dir, new_dist);
    add3(position, target, dir);
}

float Camera::compute_screen_error(float world_error, float distance) const {
    // Project world-space error to screen-space pixels
    // Screen error = (world_error / distance) * projection_scale * screen_height
    // projection_scale = cot(fov/2) for perspective projection

    if (distance < 1e-6f) {
        return 1e10f;  // Very close, always select highest LOD
    }

    float cot_half_fov = 1.0f / std::tan(radians(fov) / 2.0f);

    // Assume 1080p as reference height (could be parameterized)
    constexpr float ref_height = 1080.0f;

    float screen_error = (world_error / distance) * cot_half_fov * ref_height * 0.5f;
    return screen_error;
}

} // namespace vgeo
