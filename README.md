# VGEO - Virtualized Geometry for Blender

A Nanite-class geometry virtualization system enabling real-time rendering of billions of triangles in Blender.

## Status

🚧 **Early Development** - Specifications complete, implementation starting.

## Features (Planned)

- **Massive Scale:** Handle 10M → 1B+ triangle scenes
- **GPU-Driven:** Frustum, occlusion, and backface culling on GPU
- **Automatic LOD:** Continuous level-of-detail from cluster hierarchy
- **Streaming:** On-demand loading of geometry pages
- **Blender Integration:** Export and preview directly from Blender

## Architecture

```
Blender Addon ──► .vgeo files ──► VGEO Viewer
     │                               │
     └──────── Camera Sync ──────────┘
```

## Documentation

- [Project Specification](spec/PROJECT_SPEC.md) - Full project plan and architecture
- [VGEO Format](spec/vgeo_format.md) - Binary geometry format
- [VScene Format](spec/vscene_format.md) - Scene manifest format

## Building

```bash
# Coming soon
mkdir build && cd build
cmake ..
cmake --build .
```

## Project Structure

```
├── spec/           # Format specifications
├── src/
│   ├── core/       # Shared utilities
│   ├── preprocess/ # Mesh → VGEO converter
│   ├── runtime/    # Vulkan renderer
│   └── viewer/     # Standalone viewer app
├── tools/          # CLI utilities
├── blender/        # Blender addon
└── tests/          # Test assets and unit tests
```

## Requirements

- Vulkan 1.3+ capable GPU
- Windows 10+ / Linux (macOS via MoltenVK)
- CMake 3.20+
- C++20 compiler

## License

TBD

## Acknowledgments

Built on insights from:
- Epic Games' Nanite (Unreal Engine 5)
- meshoptimizer by Arseny Kapoulkine
- NVIDIA Mesh Shader research
