# VSCENE Manifest Format Specification v0.1

## Overview

The `.vscene` format is a JSON manifest that describes a complete scene: asset references, instances with transforms, camera settings, and metadata. It acts as the entry point for the VGEO viewer.

## Design Goals

- **Human Readable:** JSON for easy editing and debugging
- **Decoupled:** References .vgeo files, doesn't embed them
- **Extensible:** Additional properties ignored by older readers
- **Blender Compatible:** Maps to Blender's scene concepts

## Schema

```json
{
  "$schema": "https://vgeo.dev/schemas/vscene-0.1.json",
  "version": "0.1.0",
  "generator": "vgeo-blender-addon/1.0",
  "created": "2025-01-24T12:00:00Z",

  "settings": { ... },
  "assets": [ ... ],
  "instances": [ ... ],
  "camera": { ... }
}
```

## Root Properties

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| version | string | yes | Semver format version |
| generator | string | no | Tool that created this file |
| created | string | no | ISO 8601 timestamp |
| settings | object | no | Scene-wide settings |
| assets | array | yes | Asset definitions |
| instances | array | yes | Object instances |
| camera | object | no | Initial camera |

## Settings Object

```json
{
  "settings": {
    "units": "meters",
    "up_axis": "Z",
    "error_threshold": 1.0,
    "streaming_budget_mb": 1024
  }
}
```

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| units | string | "meters" | World units |
| up_axis | string | "Z" | Up direction ("Y" or "Z") |
| error_threshold | number | 1.0 | Screen-space error in pixels |
| streaming_budget_mb | number | 1024 | Max VRAM for streaming |

## Assets Array

Each asset is a .vgeo file that can be instanced.

```json
{
  "assets": [
    {
      "id": "rock_01",
      "name": "Large Rock",
      "path": "meshes/rock_01.vgeo",
      "bounds": {
        "min": [-2.5, -2.5, 0],
        "max": [2.5, 2.5, 3.0]
      }
    }
  ]
}
```

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| id | string | yes | Unique identifier |
| name | string | no | Human-readable name |
| path | string | yes | Relative path to .vgeo |
| bounds | object | no | Cached AABB |

## Instances Array

Each instance places an asset in the scene.

```json
{
  "instances": [
    {
      "id": "rock_01_instance_1",
      "asset": "rock_01",
      "transform": [
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        10, 5, 0, 1
      ],
      "visible": true,
      "tags": ["environment", "hero"]
    }
  ]
}
```

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| id | string | yes | Unique instance identifier |
| asset | string | yes | Reference to asset id |
| transform | array[16] | yes | Column-major 4x4 matrix |
| visible | boolean | no | Initial visibility (default: true) |
| tags | array | no | Arbitrary tags for filtering |

### Transform Matrix

The 16-element array is a column-major 4x4 transformation matrix:

```
[ m00, m10, m20, m30,    // Column 0 (X axis)
  m01, m11, m21, m31,    // Column 1 (Y axis)
  m02, m12, m22, m32,    // Column 2 (Z axis)
  m03, m13, m23, m33 ]   // Column 3 (Translation)
```

For a simple translation to (10, 5, 0):
```json
[1,0,0,0, 0,1,0,0, 0,0,1,0, 10,5,0,1]
```

## Camera Object

Initial viewport camera.

```json
{
  "camera": {
    "type": "perspective",
    "position": [0, -10, 5],
    "target": [0, 0, 0],
    "up": [0, 0, 1],
    "fov": 45,
    "near": 0.1,
    "far": 10000
  }
}
```

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| type | string | "perspective" | "perspective" or "orthographic" |
| position | array[3] | [0,0,10] | Camera world position |
| target | array[3] | [0,0,0] | Look-at point |
| up | array[3] | [0,0,1] | Up vector |
| fov | number | 45 | Vertical FOV in degrees |
| near | number | 0.1 | Near clip plane |
| far | number | 10000 | Far clip plane |
| ortho_scale | number | 10 | Orthographic scale (if type=orthographic) |

## Complete Example

```json
{
  "version": "0.1.0",
  "generator": "vgeo-blender-addon/1.0",
  "created": "2025-01-24T12:00:00Z",

  "settings": {
    "units": "meters",
    "up_axis": "Z",
    "error_threshold": 1.0
  },

  "assets": [
    {
      "id": "terrain",
      "name": "Terrain Chunk",
      "path": "terrain/chunk_0_0.vgeo"
    },
    {
      "id": "rock_large",
      "name": "Large Rock",
      "path": "props/rock_large.vgeo"
    },
    {
      "id": "tree_oak",
      "name": "Oak Tree",
      "path": "vegetation/tree_oak.vgeo"
    }
  ],

  "instances": [
    {
      "id": "terrain_main",
      "asset": "terrain",
      "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]
    },
    {
      "id": "rock_1",
      "asset": "rock_large",
      "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 15,20,0,1]
    },
    {
      "id": "rock_2",
      "asset": "rock_large",
      "transform": [0.7,0.7,0,0, -0.7,0.7,0,0, 0,0,1,0, -10,5,0,1]
    },
    {
      "id": "tree_1",
      "asset": "tree_oak",
      "transform": [1,0,0,0, 0,1,0,0, 0,0,1,0, 5,8,0,1],
      "tags": ["vegetation"]
    }
  ],

  "camera": {
    "type": "perspective",
    "position": [50, -50, 30],
    "target": [0, 0, 5],
    "up": [0, 0, 1],
    "fov": 60,
    "near": 0.5,
    "far": 5000
  }
}
```

## Live Sync Extension

For Blender viewport sync, the manifest can be updated in real-time or supplemented with a socket protocol:

```json
{
  "live_sync": {
    "enabled": true,
    "port": 9876,
    "protocol": "tcp"
  }
}
```

The sync protocol sends incremental updates:
- Camera transform updates
- Instance visibility changes
- Selection state

(Full sync protocol TBD in separate spec)

## Validation

A valid .vscene must:
1. Parse as valid JSON
2. Have `version` field matching "0.x.x"
3. Have `assets` array with at least one entry
4. Have `instances` array (may be empty)
5. All instance `asset` references must exist in `assets`
6. All asset `path` values must be valid relative paths
7. Transform arrays must have exactly 16 numbers
