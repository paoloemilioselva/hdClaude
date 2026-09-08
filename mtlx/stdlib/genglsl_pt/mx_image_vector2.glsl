// hdClaude genglsl_pt override of image_vector2.
//
// The stock genglsl body is one line -- `texture(tex_sampler, uv)` -- and it
// cannot be kept, for two reasons that are really one. A filename in this
// target is a handle into a shared texture array rather than a sampler of its
// own, and one `<image>` node can stand for a whole UDIM set whose tile is
// chosen from the sample's own coordinate. Both live in
// `lib/mx_hdclaude_image.glsl`; everything else here is upstream's.

#include "lib/mx_hdclaude_image.glsl"
#include "lib/$fileTransformUv"

void mx_image_vector2(HdclaudeTexture tex_sampler, int layer, vec2 defaultval, vec2 texcoord, int uaddressmode, int vaddressmode, int filtertype, int framerange, int frameoffset, int frameendaction, vec2 uv_scale, vec2 uv_offset, out vec2 result)
{
    vec2 uv = mx_transform_uv(texcoord, uv_scale, uv_offset);
    result = hdclaude_sample_image(tex_sampler, uv, vec4(defaultval, 0.0, 1.0)).rg;
}
