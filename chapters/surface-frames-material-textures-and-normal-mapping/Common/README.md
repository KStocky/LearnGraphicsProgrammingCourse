# Chapter 6 surface-frame convention

This chapter's tangent builder is an educational, deterministic CPU reference.
Generated chapter geometry and generated normal maps must use the exact
position/UV orientation, mirrored-UV handedness, bitangent reconstruction, and
green-channel convention defined by `SurfaceFrame.hpp`.

Triangle degeneracy is judged by conditioning: geometric area and the UV
determinant are compared with their edge scales. Uniformly scaling a valid
triangle therefore does not make it degenerate, while zero or nearly collinear
geometry and UVs are rejected. A vertex may accumulate only one UV handedness;
duplicate vertices along mirrored UV seams before calling the builder.

The builder is not MikkTSpace and does not claim compatibility with normal maps
authored for MikkTSpace or another external tangent convention. Importing such
content requires a matching tangent generator and is outside this milestone.
