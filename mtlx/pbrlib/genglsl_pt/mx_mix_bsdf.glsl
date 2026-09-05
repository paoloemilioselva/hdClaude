// hdClaude genglsl_pt override of mix_bsdf.
//
// Both upstream lines are unchanged. hdClaude adds the density, the direction
// selection, and the guide combination.
//
// The density line is the whole reason the closure protocol is shaped the way
// it is. `mix` must report the *mixture* density
//
//     pdf = (1 - w) * pdf_bg + w * pdf_fg
//
// evaluated at one common direction. Reporting the selected child's density
// instead is unbiased-looking and wrong: it makes every MIS weight in the
// renderer incorrect, in a way that survives convergence and cannot be seen by
// inspecting an image. Because both children are evaluated at the same
// direction before this function runs, the correct mixture is just `mix` --
// exactly as it is for the response.

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
    result.guideAlbedo = mix(bg.guideAlbedo, fg.guideAlbedo, w);
    result.guideRoughness = mix(bg.guideRoughness, fg.guideRoughness, w);

    // Direction selection under PT_SAMPLE. Both children have already sampled;
    // choose one with the authored weight. The result is delta only if the
    // branch actually taken is delta -- a mix of a mirror and a diffuse lobe is
    // not a delta closure, and treating it as one would suppress next-event
    // estimation on the diffuse half.
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        float u = closureData.u.z;
        float selectionPdf;
        if (mx_pt_select_lobe(u, w, selectionPdf))
        {
            result.sampledL = fg.sampledL;
            result.isDelta = fg.isDelta;
        }
        else
        {
            result.sampledL = bg.sampledL;
            result.isDelta = bg.isDelta;
        }
    }
    else
    {
        // Under evaluation, "delta" describes the closure as a whole: it is a
        // delta closure only if every constituent lobe is one.
        result.isDelta = min(fg.isDelta, bg.isDelta);
    }
}
