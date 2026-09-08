#version 460
#include "path_state.glsl"

// Turns GPU-written counters into indirect dispatch commands.
//
// The counters this reads -- how many paths are still active, how many shadow
// rays a bounce produced, how many paths each material claims -- are only known
// on the device. Reading them back to size the next dispatch would put a stall
// in the middle of every bounce, so a kernel writes the commands instead and
// the following dispatches are indirect (docs/wavefront-integrator.md 2).
//
// One invocation. The per-material loop is serial over the scene's distinct
// compiled programs, which is tens rather than thousands, and a serial scan of
// tens of integers is far cheaper than the barrier a parallel scan would need
// to be correct.

layout(local_size_x = 1) in;

layout(push_constant) uniform PrepareParams {
    /// Which point in the bounce this runs at; see the stages below.
    uint stage;
} prepareParams;

// Before `extend`: size the dispatches that walk the active queue, and clear
// the counts the sort is about to accumulate into.
#define HDCLAUDE_PREPARE_ACTIVE 0u
// Between the sort's two passes: turn counts into offsets, size each material's
// dispatch, and clear the scatter cursors.
#define HDCLAUDE_PREPARE_MATERIALS 1u
// After shading: size the shadow dispatch from the rays shading produced.
#define HDCLAUDE_PREPARE_SHADOW 2u

void main()
{
    if (prepareParams.stage == HDCLAUDE_PREPARE_ACTIVE)
    {
        dispatchArgs.values[HDCLAUDE_DISPATCH_ACTIVE] =
            hdclaude_dispatch_groups(counters.activeCount);

        // Every active path is about to be given a ray, so this is where they
        // are counted: once per bounce, by the one invocation that already has
        // the number in hand. A path tracer's cost is its rays, and until this
        // existed the only ray count anybody could state was the camera's --
        // the one number that needs no counting.
        counters.tracedRays += counters.activeCount;

        for (uint m = 0u; m < frame.materialCount; ++m)
        {
            materialTable.values[m] = 0u;
        }
        return;
    }

    if (prepareParams.stage == HDCLAUDE_PREPARE_MATERIALS)
    {
        // Exclusive prefix sum: material m's group starts where every earlier
        // material's group has ended, so the groups partition one buffer
        // without overlapping.
        uint running = 0u;
        for (uint m = 0u; m < frame.materialCount; ++m)
        {
            uint count = materialTable.values[m];
            materialTable.values[frame.materialCount + m] = running;
            materialTable.values[2u * frame.materialCount + m] = 0u;
            dispatchArgs.values[HDCLAUDE_DISPATCH_MATERIAL + m] =
                hdclaude_dispatch_groups(count);
            running += count;
        }
        return;
    }

    dispatchArgs.values[HDCLAUDE_DISPATCH_SHADOW] =
        hdclaude_dispatch_groups(counters.shadowCount);

    // And the shadow rays shading produced, counted where their dispatch is
    // sized. Kept apart from the traced count because they are a different
    // question: one is how far paths got, the other how much light was sampled.
    counters.shadowRays += counters.shadowCount;
}
