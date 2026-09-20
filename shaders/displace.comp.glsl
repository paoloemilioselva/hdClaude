// The displace kernel body.
//
// This text is appended to a *generated MaterialX displacement program*, so it
// sees that program's ABI: `hdclaude_set_surface_hit`,
// `hdclaude_material_displace`, and the `hdclaude_displacement` struct the
// program leaves its answer in. It is the geometry counterpart of
// shade.comp.glsl, which is appended to the *surface* program of the same
// material.
//
// One invocation per vertex, not per path and not per triangle. A vertex is
// displaced once because it can only be in one place; see
// include/hdclaude/gpu/vertex_frames.h for why the frame it is displaced with
// is accumulated over the triangles around it.
//
// The material module already emits `#version` and its extension directives,
// so this file must not: GLSL requires them before any code, and there is code
// above. `GL_EXT_buffer_reference` is the one it does not emit, because
// nothing in a generated material takes a device address.

#extension GL_EXT_buffer_reference : require

layout(local_size_x = 64) in;

// Geometry reaches this kernel as device addresses rather than as descriptors.
// A displacement runs once per prototype rather than once per frame, so there
// is no per-frame descriptor set to put a prototype's buffers in, and pushing
// the addresses means one pipeline serves every mesh the material displaces
// without a descriptor write between them.
layout(buffer_reference, scalar) readonly buffer DisplacePoints  { vec3 values[]; };
layout(buffer_reference, scalar) readonly buffer DisplaceVectors { vec3 values[]; };
layout(buffer_reference, scalar) readonly buffer DisplaceUvs     { vec2 values[]; };
layout(buffer_reference, scalar) writeonly buffer DisplaceResult { vec3 values[]; };

layout(push_constant) uniform DisplaceParams {
    /// Object-space positions, one vec3 per vertex. The mesh as authored and
    /// refined, before anything moved.
    uint64_t positions;
    /// Per-vertex shading normals, dP/du and dP/dv, from ComputeVertexFrames.
    /// Any of them may be zero, in which case the frame falls back below.
    uint64_t normals;
    uint64_t dpdu;
    uint64_t dpdv;
    /// Per-vertex texture coordinates, or zero when the mesh has none.
    uint64_t uvs;
    /// Where the displaced positions go. Separate from `positions` so the
    /// undisplaced mesh survives -- a refit needs it, and a displacement that
    /// changed would otherwise be applied on top of the previous one.
    uint64_t displaced;

    uint vertexCount;
    /// hdclaude::DisplacementSpace. Which of MaterialX's two constructors the
    /// program was compiled from, which is the only thing that says what its
    /// three floats mean; see include/hdclaude/gpu/scene.h.
    uint space;
} displaceParams;

void main()
{
    const uint vertex = gl_GlobalInvocationID.x;
    if (vertex >= displaceParams.vertexCount)
    {
        return;
    }

    const vec3 P = DisplacePoints(displaceParams.positions).values[vertex];

    vec3 N = vec3(0.0, 0.0, 1.0);
    if (displaceParams.normals != 0ul)
    {
        const vec3 authored = DisplaceVectors(displaceParams.normals).values[vertex];
        if (dot(authored, authored) > 0.0)
        {
            N = normalize(authored);
        }
    }

    vec2 uv = vec2(0.0);
    if (displaceParams.uvs != 0ul)
    {
        uv = DisplaceUvs(displaceParams.uvs).values[vertex];
    }

    // --- The tangent frame ---------------------------------------------------
    //
    // Built here rather than on the host, and built exactly as
    // shade.comp.glsl builds it: the tangent is dP/du orthogonalised against
    // the shading normal, and the bitangent's handedness is read off dP/dv so
    // a mirrored UV island keeps its orientation. A material that is displaced
    // and then shaded has to see one frame, and the surest way to get one is
    // for there to be one piece of arithmetic.
    vec3 dpdu = vec3(0.0);
    vec3 dpdv = vec3(0.0);
    if (displaceParams.dpdu != 0ul)
    {
        dpdu = DisplaceVectors(displaceParams.dpdu).values[vertex];
    }
    if (displaceParams.dpdv != 0ul)
    {
        dpdv = DisplaceVectors(displaceParams.dpdv).values[vertex];
    }

    vec3 T = dpdu - N * dot(N, dpdu);
    if (!(dot(T, T) > 1.0e-20))
    {
        // dP/du parallel to the normal, or absent; any orthogonal direction
        // will do, and the same one the shade kernel would have chosen.
        const vec3 fallback = abs(N.z) < 0.9 ? vec3(0.0, 0.0, 1.0)
                                             : vec3(1.0, 0.0, 0.0);
        T = cross(fallback, N);
    }
    T = normalize(T);
    vec3 B = cross(N, T);
    B = dot(B, dpdv) < 0.0 ? -B : B;

    // --- Evaluate ------------------------------------------------------------
    //
    // Object space is handed over as both the world and the object frame, and
    // that is a statement about what a prototype is rather than a shortcut. A
    // prototype is object-space geometry shared by every placement of it, so a
    // displacement evaluated in world space would be a different mesh per
    // instance and the sharing -- which is what makes an instanced asset fit in
    // memory at all -- would be gone. A graph that asks for world space
    // therefore reads object space, and the material compiler says so.
    hdclaude_set_surface_hit(P, N, T, B, P, N, T, B, uv);
    hdclaude_material_displace();

    const vec3 offset = hdclaude_displacement.offset * hdclaude_displacement.scale;

    vec3 displaced = P;
    if (displaceParams.space == 1u)
    {
        // ND_displacement_vector3: "vector displacement in (dPdu, dPdv, N)
        // tangent/normal space", in that order.
        displaced += offset.x * T + offset.y * B + offset.z * N;
    }
    else
    {
        // ND_displacement_float: "scalar displacement amount along the surface
        // normal direction", packed by mx_displacement_float as vec3(disp), so
        // all three components are the same number and any of them is it.
        displaced += offset.x * N;
    }

    DisplaceResult(displaceParams.displaced).values[vertex] = displaced;
}
