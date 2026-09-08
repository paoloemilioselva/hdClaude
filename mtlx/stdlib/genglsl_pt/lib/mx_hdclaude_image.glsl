// hdClaude's texture lookup, shared by every genglsl_pt `image` override.
//
// The stock genglsl implementations take a `sampler2D` and call
// `texture(tex_sampler, uv)`. hdClaude cannot, because a filename in this
// target is a *handle* into one shared texture array rather than a sampler of
// its own (see src/materialx/pathtracer_generator.cpp), and because one
// `<image>` node can stand for a whole UDIM set.
//
// UDIM, as USD and MaterialX leave it: a `<UDIM>` token in the path names a
// 10x10 grid of images laid out over UV space, tile
//
//     1001 + floor(u) + 10 * floor(v)
//
// so tile 1001 covers u in [0, 1) and v in [0, 1), 1002 the square to its
// right, 1011 the square above 1001. The renderer is expected to expand the
// token and pick the tile each sample lands in. Selection has to happen here,
// per sample, and not per mesh: ALab's `discPin_M_geo` straddles tiles 1002
// and 1013 on its own.
//
// The set's tiles occupy a contiguous run of array slots in ascending tile
// order, and the handle carries where that run starts and which tiles are in
// it. A sample landing outside the grid, or on a tile the set does not ship,
// reads the node's default rather than an arbitrary neighbour -- there is no
// image there, and inventing one is how a missing tile becomes a plausible
// wrong picture.

vec4 hdclaude_sample_image(HdclaudeTexture handle, vec2 uv, vec4 fallback)
{
    int slot = handle.slot;

    if (handle.tileCount > 1)
    {
        vec2 tileUv = floor(uv);
        if (tileUv.x < 0.0 || tileUv.x > 9.0 || tileUv.y < 0.0 || tileUv.y > 9.0)
        {
            return fallback;
        }
        int wanted = 1001 + int(tileUv.x) + 10 * int(tileUv.y);

        slot = -1;
        for (int i = 0; i < handle.tileCount; ++i)
        {
            if (hdclaude_udim_tiles[handle.tileOffset + i] == wanted)
            {
                slot = handle.slot + i;
                break;
            }
        }
        if (slot < 0)
        {
            return fallback;
        }

        // Into the tile's own unit square. Not `fract`, which folds a negative
        // coordinate the wrong way; the tile was chosen from the same floor.
        uv -= tileUv;
    }

    return texture(hdclaude_textures[slot], uv);
}
