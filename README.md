# Learn Graphics Programming

Published learner version: `course-v2026.08.19.3`

This is a complete standalone learner repository. It contains the framework,
Starter and Solution projects, assets, and cumulative teaching patches for every
chapter available in `course-v2026.08.19.3`. It does not require a checkout of the private
authoring repository.

## Requirements

- Windows 11 x64
- Visual Studio with Desktop development with C++
- CMake 4.1 or newer
- Optional: LLVM clang-cl

Pinned public dependencies are downloaded by CMake. First-party C++ uses C++23
and builds with `/W4 /WX /EHsc` under MSVC and clang-cl.

## Configure and build

```powershell
cmake --preset msvc
cmake --build --preset msvc-debug
```

Visual Studio can open the repository folder directly and use the
`visual-studio` preset. Tests and private authoring validation are intentionally
not included in this learner distribution.

## Available chapters

- [Graphics Mathematics and Coordinate Transformations](chapters/graphics-math-coordinate-transforms)
- [Rasterization, Interpolation, Depth, and Culling](chapters/rasterization-depth-and-culling)
- [Texture Sampling, Mipmapping, and Reconstruction](chapters/texture-sampling-mipmapping-and-reconstruction)
- [Color Spaces, HDR, Exposure, and Tone Mapping](chapters/color-spaces-hdr-exposure-and-tone-mapping)
- [Radiometry, Light Units, and BRDFs](chapters/radiometry-light-units-and-brdfs)
- [Surface Frames, Material Textures, and Normal Mapping](chapters/surface-frames-material-textures-and-normal-mapping)
- [Shadow-Map Fundamentals, Precision, and Bias](chapters/shadow-map-fundamentals-and-bias)
- [Pass Graph Fundamentals and Resource Lifetimes](chapters/frame-graph-and-resource-lifetimes)
- [Transient Resource Allocation and Aliasing Fundamentals](chapters/transient-resource-allocation-and-aliasing)
- [Pass Scheduling and Multi-Queue Execution](chapters/pass-scheduling-and-multi-queue-execution)
- [GPU Work Distribution and Indirect Execution](chapters/gpu-work-distribution-and-indirect-execution)
- [Motion Vectors, Reprojection, and History Validation](chapters/motion-vectors-and-reprojection)
- [G-Buffers and Deferred Shading](chapters/gbuffer-and-deferred-shading)
- [Tiled, Forward+, and Clustered Lighting](chapters/tiled-forward-plus-and-clustered-lighting)

The matching course site and tagged source archive use the same `course-v2026.08.19.3`
identifier. Report learner-facing problems in this repository's Issues page.
