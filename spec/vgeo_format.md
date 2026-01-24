# VGEO Binary Format Specification v0.1

## Overview

The `.vgeo` format stores virtualized geometry data optimized for GPU-driven rendering. It contains meshlets, cluster hierarchies, and associated metadata in a chunked binary layout.

## Design Goals

- **Streamable:** Fixed-size pages, independent chunks
- **GPU-Ready:** Data layouts match GPU buffer requirements
- **Compact:** Quantized attributes, optional compression
- **Extensible:** Chunk-based structure allows future additions
- **Validatable:** Magic numbers, checksums, clear structure

## File Layout

```
┌─────────────────────────────────────────────────────────┐
│ File Header (64 bytes, fixed)                           │
├─────────────────────────────────────────────────────────┤
│ Chunk Directory (variable)                              │
├─────────────────────────────────────────────────────────┤
│ Chunk Data (variable, aligned to 16 bytes)              │
│   ├── VERT chunk                                        │
│   ├── NORM chunk                                        │
│   ├── UVCО chunk                                        │
│   ├── INDX chunk                                        │
│   ├── MSLT chunk                                        │
│   ├── CLST chunk                                        │
│   ├── BVOL chunk                                        │
│   └── CONE chunk                                        │
└─────────────────────────────────────────────────────────┘
```

## Header Structure

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | char[4] | magic | "VGEO" |
| 4 | 2 | u16 | version_major | Format major version |
| 6 | 2 | u16 | version_minor | Format minor version |
| 8 | 4 | u32 | flags | See Flags section |
| 12 | 4 | u32 | meshlet_count | Total meshlets |
| 16 | 4 | u32 | cluster_count | Total clusters in hierarchy |
| 20 | 4 | u32 | vertex_count | Total vertices |
| 24 | 4 | u32 | index_count | Total triangle indices |
| 28 | 4 | u32 | chunk_count | Number of chunks |
| 32 | 24 | AABB | bounds | World-space bounding box |
| 56 | 8 | u8[8] | reserved | Reserved for future use |

### AABB Structure (24 bytes)

| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0 | 12 | f32[3] | min |
| 12 | 12 | f32[3] | max |

### Flags (bitfield)

| Bit | Name | Description |
|-----|------|-------------|
| 0 | COMPRESSED | Chunk data is LZ4 compressed |
| 1 | QUANTIZED_POS | Positions are quantized (see VERT) |
| 2 | HAS_NORMALS | NORM chunk present |
| 3 | HAS_UVS | UVCО chunk present |
| 4 | HAS_TANGENTS | TANG chunk present |
| 5 | 16BIT_INDICES | Indices are u16 (else u32) |
| 6-31 | reserved | |

## Chunk Directory

Immediately follows header. Each entry is 16 bytes.

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | char[4] | type | Chunk type identifier |
| 4 | 4 | u32 | offset | Byte offset from file start |
| 8 | 4 | u32 | size | Uncompressed size in bytes |
| 12 | 4 | u32 | compressed_size | Compressed size (0 if uncompressed) |

## Chunk Types

### VERT - Vertex Positions

Contains vertex position data for all meshlets.

**Unquantized (flags.QUANTIZED_POS = 0):**
```c
struct Vertex {
    float x, y, z;  // 12 bytes
};
```

**Quantized (flags.QUANTIZED_POS = 1):**
```c
struct QuantizationParams {
    float origin[3];    // Bounding box min
    float scale[3];     // Bounding box size
    uint8_t bits;       // Bits per component (16 or 21)
    uint8_t padding[3];
};
// Followed by packed quantized positions
// 16-bit: 6 bytes per vertex (u16 x 3)
// 21-bit: 8 bytes per vertex (packed into u64)
```

### NORM - Vertex Normals

Octahedral-encoded normals for compact storage.

```c
struct OctNormal {
    int16_t x, y;  // snorm16, z derived from x,y
};
```

Decoding: `z = 1.0 - abs(x) - abs(y)`, then normalize.

### UVCО - Texture Coordinates

```c
struct UV {
    uint16_t u, v;  // half-float (f16)
};
```

### INDX - Triangle Indices

Local indices within each meshlet. Index size determined by flags.16BIT_INDICES.

```c
// If 16BIT_INDICES: u16 per index
// Else: u32 per index
// 3 indices per triangle
```

### MSLT - Meshlet Descriptors

Defines each meshlet's data ranges.

```c
struct MeshletDescriptor {
    uint32_t vertex_offset;    // Offset into VERT chunk
    uint32_t vertex_count;     // Number of vertices
    uint32_t index_offset;     // Offset into INDX chunk
    uint32_t triangle_count;   // Number of triangles
    uint32_t cluster_id;       // Parent cluster in hierarchy
};
```

### CLST - Cluster Hierarchy

DAG structure for LOD selection.

```c
struct ClusterNode {
    uint32_t parent_id;         // Parent cluster (0xFFFFFFFF if root)
    uint32_t child_start;       // First child index
    uint32_t child_count;       // Number of children
    uint32_t meshlet_start;     // First meshlet (leaf nodes only)
    uint32_t meshlet_count;     // Meshlets in this cluster
    float error;                // Geometric error metric
    float parent_error;         // Parent's error (for LOD cut)
    uint8_t lod_level;          // 0 = highest detail
    uint8_t padding[3];
};
```

### BVOL - Bounding Volumes

Per-meshlet bounding spheres for culling.

```c
struct BoundingSphere {
    float center[3];
    float radius;
};
```

### CONE - Normal Cones

Per-meshlet normal cones for backface culling.

```c
struct NormalCone {
    int8_t axis[3];    // Normalized axis (snorm8)
    int8_t cos_angle;  // cos(aperture/2) as snorm8
};
```

Culling test: `dot(view_dir, axis) < cos_angle → cull`

## Alignment Requirements

- All chunks start at 16-byte aligned offsets
- Chunk data may have internal alignment (e.g., vertex data at 4-byte)
- Padding bytes should be zero

## Compression

When flags.COMPRESSED is set:
- Each chunk is independently compressed with LZ4
- compressed_size field indicates compressed bytes
- Decompression yields `size` bytes

## Versioning

- **Major version change:** Breaking format changes
- **Minor version change:** Additive changes (new chunks, flags)

Current version: 0.1

## Example

A simple cube (8 vertices, 12 triangles, 1 meshlet):

```
Header:
  magic = "VGEO"
  version = 0.1
  flags = 0x06 (HAS_NORMALS | HAS_UVS)
  meshlet_count = 1
  vertex_count = 8
  index_count = 36

Chunks:
  VERT: 8 * 12 = 96 bytes
  NORM: 8 * 4 = 32 bytes
  UVCО: 8 * 4 = 32 bytes
  INDX: 36 * 4 = 144 bytes
  MSLT: 1 * 20 = 20 bytes
  BVOL: 1 * 16 = 16 bytes
  CONE: 1 * 4 = 4 bytes

Total: 64 (header) + 112 (directory) + 344 (data) = 520 bytes
```
