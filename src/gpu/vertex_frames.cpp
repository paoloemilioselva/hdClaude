#include "hdclaude/gpu/vertex_frames.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace hdclaude {

namespace {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Vec3 Load(const std::vector<float>& values, std::size_t index)
{
    return {values[index * 3 + 0], values[index * 3 + 1],
            values[index * 3 + 2]};
}

void Add(std::vector<float>& values, std::size_t index, const Vec3& v,
         float weight)
{
    values[index * 3 + 0] += v.x * weight;
    values[index * 3 + 1] += v.y * weight;
    values[index * 3 + 2] += v.z * weight;
}

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

float Length(const Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vec3 Scale(const Vec3& v, float s) { return {v.x * s, v.y * s, v.z * s}; }

Vec3 Combine(const Vec3& a, float sa, const Vec3& b, float sb)
{
    return {a.x * sa + b.x * sb, a.y * sa + b.y * sb, a.z * sa + b.z * sb};
}

/// The texture coordinate at one corner of one triangle, or the barycentric
/// parameterisation when the prototype has none.
///
/// The fallback matches `shade.comp.glsl`: a mesh with no coordinates is
/// parameterised by the triangle's own barycentrics, so the same arithmetic
/// produces a frame for it instead of needing a branch of its own.
std::array<float, 2> CornerUv(const MeshPrototype& prototype,
                              std::size_t triangle, std::size_t corner,
                              std::uint32_t vertex)
{
    if (prototype.uvs.empty()) {
        constexpr std::array<std::array<float, 2>, 3> kBarycentric = {
            {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}}};
        return kBarycentric[corner];
    }
    const std::size_t index =
        prototype.uvsPerCorner ? triangle * 3 + corner : vertex;
    if ((index + 1) * 2 > prototype.uvs.size()) {
        return {0.0f, 0.0f};
    }
    return {prototype.uvs[index * 2 + 0], prototype.uvs[index * 2 + 1]};
}

}  // namespace

VertexFrames ComputeVertexFrames(const MeshPrototype& prototype)
{
    VertexFrames frames;
    const std::size_t vertexCount = prototype.VertexCount();
    const std::size_t triangleCount = prototype.TriangleCount();
    if (prototype.IsCurve() || vertexCount == 0 || triangleCount == 0) {
        return frames;
    }

    frames.normals.assign(vertexCount * 3, 0.0f);
    frames.dpdu.assign(vertexCount * 3, 0.0f);
    frames.dpdv.assign(vertexCount * 3, 0.0f);

    // Authored per-vertex normals are the mesh's own answer and are used as
    // they are. Anything else -- face-varying, or none at all -- is summed
    // below, because a vertex can only move in one direction.
    const bool haveVertexNormals = !prototype.normals.empty() &&
                                   !prototype.normalsPerCorner &&
                                   prototype.normals.size() >= vertexCount * 3;
    if (haveVertexNormals) {
        frames.normals.assign(prototype.normals.begin(),
                              prototype.normals.begin() +
                                  static_cast<std::ptrdiff_t>(vertexCount * 3));
    }

    const bool haveCornerNormals =
        prototype.normalsPerCorner &&
        prototype.normals.size() >= triangleCount * 9;

    if (!prototype.uvs.empty()) {
        frames.uvs.assign(vertexCount * 2, 0.0f);
    }
    // Which vertices have already taken a coordinate, so a seam keeps the
    // first one authored for it rather than the last.
    std::vector<bool> uvAssigned(frames.uvs.empty() ? 0 : vertexCount, false);

    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
        const std::uint32_t i0 = prototype.indices[triangle * 3 + 0];
        const std::uint32_t i1 = prototype.indices[triangle * 3 + 1];
        const std::uint32_t i2 = prototype.indices[triangle * 3 + 2];
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) {
            continue;
        }

        const Vec3 p0 = Load(prototype.positions, i0);
        const Vec3 p1 = Load(prototype.positions, i1);
        const Vec3 p2 = Load(prototype.positions, i2);
        const Vec3 e1 = Subtract(p1, p0);
        const Vec3 e2 = Subtract(p2, p0);
        const Vec3 faceNormal = Cross(e1, e2);
        // Twice the triangle's area, which is the weight and is already here.
        const float weight = Length(faceNormal);
        if (!(weight > 0.0f)) {
            continue;
        }

        const std::array<float, 2> uv0 = CornerUv(prototype, triangle, 0, i0);
        const std::array<float, 2> uv1 = CornerUv(prototype, triangle, 1, i1);
        const std::array<float, 2> uv2 = CornerUv(prototype, triangle, 2, i2);
        const float du1 = uv1[0] - uv0[0];
        const float dv1 = uv1[1] - uv0[1];
        const float du2 = uv2[0] - uv0[0];
        const float dv2 = uv2[1] - uv0[1];
        const float determinant = du1 * dv2 - du2 * dv1;

        Vec3 dpdu;
        Vec3 dpdv;
        if (std::fabs(determinant) > 1.0e-20f) {
            const float inverse = 1.0f / determinant;
            dpdu = Combine(e1, dv2 * inverse, e2, -dv1 * inverse);
            dpdv = Combine(e2, du1 * inverse, e1, -du2 * inverse);
        } else {
            // A collapsed UV triangle says nothing about direction, so the
            // edge stands in -- the same fallback the shade kernel takes, so
            // the two agree about a degenerate parameterisation as well as
            // about a good one.
            dpdu = e1;
            dpdv = Cross(faceNormal, e1);
        }

        // Each contribution is weighted by twice the triangle's area. The
        // derivatives are a rate rather than a direction, so they are scaled
        // by the weight explicitly where the face normal already carries it in
        // its length.
        for (const std::uint32_t vertex : {i0, i1, i2}) {
            Add(frames.dpdu, vertex, dpdu, weight);
            Add(frames.dpdv, vertex, dpdv, weight);
            if (!haveVertexNormals && !haveCornerNormals) {
                Add(frames.normals, vertex, faceNormal, 1.0f);
            }
        }
        if (haveCornerNormals) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t vertex =
                    prototype.indices[triangle * 3 + corner];
                Add(frames.normals, vertex,
                    Load(prototype.normals, triangle * 3 + corner), weight);
            }
        }

        if (!frames.uvs.empty()) {
            const std::array<std::array<float, 2>, 3> uvs = {uv0, uv1, uv2};
            const std::array<std::uint32_t, 3> corners = {i0, i1, i2};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                const std::uint32_t vertex = corners[corner];
                if (uvAssigned[vertex]) {
                    continue;
                }
                uvAssigned[vertex] = true;
                frames.uvs[vertex * 2 + 0] = uvs[corner][0];
                frames.uvs[vertex * 2 + 1] = uvs[corner][1];
            }
        }
    }

    // Only the normal is normalised here. The derivatives keep their scale,
    // because the kernel builds the same frame `shade.comp.glsl` builds and
    // that arithmetic reads their length and their handedness.
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex) {
        const Vec3 n = Load(frames.normals, vertex);
        const float length = Length(n);
        if (length > 1.0e-20f) {
            const Vec3 unit = Scale(n, 1.0f / length);
            frames.normals[vertex * 3 + 0] = unit.x;
            frames.normals[vertex * 3 + 1] = unit.y;
            frames.normals[vertex * 3 + 2] = unit.z;
        }
    }

    return frames;
}

}  // namespace hdclaude
