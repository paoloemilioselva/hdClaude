#include "hdclaude/core/sphere_mesh.h"

#include <cmath>

namespace hdclaude {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool TooCoarse(int numRadial, int numAxial)
{
    return numRadial < kMinSphereRadial || numAxial < kMinSphereAxial;
}

}  // namespace

std::size_t SphereMeshPointCount(int numRadial, int numAxial)
{
    if (TooCoarse(numRadial, numAxial)) {
        return 0;
    }
    // The sweep is closed, so the first and last point of every ring are the
    // same point: a ring has `numRadial` points, not `numRadial + 1`.
    return static_cast<std::size_t>(numRadial) *
               static_cast<std::size_t>(numAxial - 1) +
           2;
}

std::size_t SphereMeshFaceCount(int numRadial, int numAxial)
{
    if (TooCoarse(numRadial, numAxial)) {
        return 0;
    }
    const std::size_t radial = static_cast<std::size_t>(numRadial);
    const std::size_t strips = static_cast<std::size_t>(numAxial - 2);
    return radial * strips + 2 * radial;
}

SphereMesh GenerateSphereMesh(int numRadial, int numAxial, double radius)
{
    SphereMesh mesh;
    if (TooCoarse(numRadial, numAxial)) {
        return mesh;
    }

    const int radial = numRadial;
    const int axial = numAxial;
    const int rings = axial - 1;

    // --- Points ---------------------------------------------------------------
    //
    // Bottom pole, then one ring per axial division, then the top pole. The
    // order is `GeomUtilSphereMeshGenerator`'s, so that at the default density
    // this cage is indistinguishable from the one hdClaude used to get.
    mesh.points.reserve(SphereMeshPointCount(radial, axial) * 3);

    const auto write = [&mesh](double x, double y, double z) {
        mesh.points.push_back(static_cast<float>(x));
        mesh.points.push_back(static_cast<float>(y));
        mesh.points.push_back(static_cast<float>(z));
    };

    write(0.0, 0.0, -radius);
    for (int ring = 1; ring <= rings; ++ring) {
        // Latitude runs the open interval (-pi/2, pi/2): the poles are the
        // two points written outside this loop, and a ring at either end
        // would be a second copy of one.
        const double latitude =
            (static_cast<double>(ring) / static_cast<double>(axial) - 0.5) * kPi;
        const double ringRadius = radius * std::cos(latitude);
        const double height = radius * std::sin(latitude);
        for (int step = 0; step < radial; ++step) {
            const double longitude = (static_cast<double>(step) /
                                      static_cast<double>(radial)) *
                                     2.0 * kPi;
            write(ringRadius * std::cos(longitude),
                  ringRadius * std::sin(longitude), height);
        }
    }
    write(0.0, 0.0, radius);

    const int topPole = static_cast<int>(mesh.PointCount()) - 1;

    // --- Faces and their coordinates -----------------------------------------
    //
    // The texture coordinate of a *corner* rather than of a vertex, which is
    // what lets the seam close: the corner that wraps past the last division
    // is given u = 1 where the vertex it refers to carries u = 0.
    mesh.faceVertexCounts.reserve(SphereMeshFaceCount(radial, axial));
    mesh.faceVertexIndices.reserve(SphereMeshFaceCount(radial, axial) * 4);
    mesh.faceVaryingUvs.reserve(SphereMeshFaceCount(radial, axial) * 8);

    const auto u = [radial](int step) {
        return static_cast<float>(static_cast<double>(step) /
                                  static_cast<double>(radial));
    };
    const auto v = [axial](int ring) {
        return static_cast<float>(static_cast<double>(ring) /
                                  static_cast<double>(axial));
    };
    const auto corner = [&mesh](int index, float uu, float vv) {
        mesh.faceVertexIndices.push_back(index);
        mesh.faceVaryingUvs.push_back(uu);
        mesh.faceVaryingUvs.push_back(vv);
    };

    // The first point of ring `ring`, counting rings from one.
    const auto ringStart = [radial](int ring) { return 1 + (ring - 1) * radial; };

    // Bottom fan. The winding is the generator's: next, current, pole.
    for (int step = 0; step < radial; ++step) {
        mesh.faceVertexCounts.push_back(3);
        corner(ringStart(1) + (step + 1) % radial, u(step + 1), v(1));
        corner(ringStart(1) + step, u(step), v(1));
        // The pole is one point shared by every face of the fan, so its
        // coordinate is whatever each face needs: the middle of the span that
        // face covers, which keeps the texture from shearing round it.
        corner(0, u(step) + 0.5f / static_cast<float>(radial), v(0));
    }

    // Middle quads, one strip between each pair of neighbouring rings.
    for (int ring = 1; ring + 1 <= rings; ++ring) {
        const int lower = ringStart(ring);
        const int upper = ringStart(ring + 1);
        for (int step = 0; step < radial; ++step) {
            const int next = (step + 1) % radial;
            mesh.faceVertexCounts.push_back(4);
            corner(lower + step, u(step), v(ring));
            corner(lower + next, u(step + 1), v(ring));
            corner(upper + next, u(step + 1), v(ring + 1));
            corner(upper + step, u(step), v(ring + 1));
        }
    }

    // Top fan, off the last ring.
    const int last = ringStart(rings);
    for (int step = 0; step < radial; ++step) {
        mesh.faceVertexCounts.push_back(3);
        corner(last + step, u(step), v(rings));
        corner(last + (step + 1) % radial, u(step + 1), v(rings));
        corner(topPole, u(step) + 0.5f / static_cast<float>(radial), v(axial));
    }

    return mesh;
}

}  // namespace hdclaude
