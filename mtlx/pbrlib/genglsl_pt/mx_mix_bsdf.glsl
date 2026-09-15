// hdClaude genglsl_pt override of mix_bsdf.
//
// MaterialX 1.39.3's body is reproduced verbatim; hdClaude appends the density,
// the direction selection, and the guide combination. 1.39.3 is the version
// inside OpenUSD 26.03, and its throughput semantics differ from 1.39.6's --
// see docs/implementation-notes.md.
//
// The density line is the whole reason the closure protocol is shaped the way
// it is. `mix` must report the *mixture* density
//
//     pdf = (1 - w) * pdf_bg + w * pdf_fg
//
// evaluated at one common direction. Reporting the selected child's density
// instead is unbiased-looking and wrong: it makes every MIS weight in the
// renderer incorrect, in a way that survives convergence and cannot be seen by
// inspecting an image. Because both children were evaluated at the same
// direction before this function runs, the correct mixture is just `mix` --
// exactly as it is for the response.
//
// An interior medium is not mixed but *selected*, with the same draw as the
// direction. A path enters one interior or the other, never a blend of the two,
// and this is the combinator that decides which: `open_pbr_surface` mixes a
// subsurface lobe against its diffuse base on `subsurface_weight`, so a
// material that authors no subsurface selects the base every time and never
// carries the medium `subsurface_bsdf` published for it.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_mix_bsdf(ClosureData closureData, BSDF fg, BSDF bg, float mixValue, out BSDF result)
{
    result.response = mix(bg.response, fg.response, mixValue);
    result.throughput = mix(bg.throughput, fg.throughput, mixValue);

    // ---- hdClaude ----------------------------------------------------------
    float w = clamp(mixValue, 0.0, 1.0);

    // Densities mix with the same weights as responses, at the same direction.
    result.pdf = mix(bg.pdf, fg.pdf, w);
    result.spectrum = mix(bg.spectrum, fg.spectrum, w);

    // Guides follow the mixture too, so a reconstruction backend sees the
    // albedo of what is actually visible rather than of one arbitrary lobe.
    result.guideDiffuse = mix(bg.guideDiffuse, fg.guideDiffuse, w);
    result.guideSpecular = mix(bg.guideSpecular, fg.guideSpecular, w);
    result.guideNormal = mix(bg.guideNormal, fg.guideNormal, w);
    result.guideRoughness = mix(bg.guideRoughness, fg.guideRoughness, w);

    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        // Both children have already sampled; choose one with the authored
        // weight. The result is delta only if the branch actually taken is:
        // a mix of a mirror and a diffuse lobe is not a delta closure, and
        // treating it as one would suppress NEE on the diffuse half.
        float u = mx_pt_selection_random();
        float selectionPdf;
        if (mx_pt_select_lobe(u, w, selectionPdf))
        {
            result.sampledL = fg.sampledL;
            result.isDelta = fg.isDelta;
            hdclaude_carry_medium(result, fg);
        }
        else
        {
            result.sampledL = bg.sampledL;
            result.isDelta = bg.isDelta;
            hdclaude_carry_medium(result, bg);
        }
    }
    else
    {
        // Under evaluation, "delta" describes the closure as a whole: it is a
        // delta closure only if every constituent lobe is one.
        result.isDelta = min(fg.isDelta, bg.isDelta);
        // Which interior a shadow ray through this surface crosses, chosen
        // as the sampling pass would have chosen it; see
        // hdclaude_select_medium.
        hdclaude_select_medium(result, fg, w * fg.pdf, bg, (1.0 - w) * bg.pdf,
                               mx_pt_selection_random());
    }
}
