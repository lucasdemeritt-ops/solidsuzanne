#pragma once

#include <array>

namespace vgeo {

struct Camera {
    float position[3] = {0, 0, 10};
    float target[3] = {0, 0, 0};
    float up[3] = {0, 1, 0};

    float fov = 45.0f;        // Degrees
    float near_plane = 0.1f;
    float far_plane = 10000.0f;
    float aspect = 16.0f / 9.0f;

    // Computed matrices (column-major)
    float view[16];
    float projection[16];
    float view_projection[16];

    // Frustum planes (nx, ny, nz, d) for culling
    float frustum_planes[6][4];

    // Update matrices and frustum from current state
    void update();

    // Orbit controls
    void orbit(float delta_yaw, float delta_pitch);
    void pan(float delta_x, float delta_y);
    void zoom(float delta);

    // Screen-space error calculation
    float compute_screen_error(float world_error, float distance) const;
};

} // namespace vgeo
