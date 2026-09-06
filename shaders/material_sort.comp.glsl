#version 460
#include "path_state.glsl"

// The counting sort that groups active paths by the material that shades them.
//
// Two passes over the active queue with a prefix sum between them, which
// prepare_dispatch performs:
//
//   pass 0   count how many paths each material claims
//   pass 1   scatter path indices into that material's group
//
// This is what makes the per-material dispatch worth having. Without it every
// shading pipeline is dispatched over every active path and discards the ones
// that are not its own, so a scene's shading cost is the number of materials
// times the number of paths -- the frame-wide cost the wavefront design exists
// to avoid (docs/architecture.md 2).
//
// Paths that missed geometry are counted by nobody: the environment kernel
// consumes them straight off the active queue.

layout(local_size_x = 64) in;

layout(push_constant) uniform SortParams {
    uint pass;
} sortParams;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];

    ivec4 record = hits.values[path];
    if (record.x < 0)
    {
        return;   // missed; not a shading path
    }

    uint material = hdclaude_material_of(instances.values[record.x], record.y);
    if (material >= frame.materialCount)
    {
        // A material index with no compiled pipeline. It cannot be shaded, and
        // counting it would reserve a group nothing dispatches over, so the
        // path is dropped here rather than left to index past the table.
        return;
    }

    if (sortParams.pass == 0u)
    {
        atomicAdd(materialTable.values[material], 1u);
        return;
    }

    // The cursors live after the counts and the offsets, and are reset by the
    // prefix-sum stage, so this pass hands out consecutive slots within the
    // group the counting pass sized.
    uint cursor = atomicAdd(
        materialTable.values[2u * frame.materialCount + material], 1u);
    materialQueue.values[hdclaude_material_offset(material) + cursor] = path;
}
