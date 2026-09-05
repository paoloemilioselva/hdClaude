"""Generate hdClaude's genglsl_pt implementation declarations from the stock ones.

Usage:
    python tools/generate_pt_implementations.py         <usd>/libraries/pbrlib/genglsl/pbrlib_genglsl_impl.mtlx         mtlx/pbrlib/genglsl_pt/hdclaude_pbrlib_impl.mtlx

Re-run this after adding an override to IMPLEMENTED below, and after any
MaterialX version change. The output is committed; this script exists so the
nodedef names in it are never retyped.

Nodedef names do not follow file names -- `mx_multiply_bsdf_color3.glsl` is
declared for `ND_multiply_bsdfC` -- and a declaration naming a nodedef that does
not exist is ignored *silently*: MaterialX falls back to the stock
implementation, and the only symptom is a duplicate `struct ClosureData` at GLSL
compile time. Deriving our declarations from the stock file removes that class
of error entirely.
"""
import io, os, re, sys

stock_impl, out_path = sys.argv[1], sys.argv[2]

# The files hdClaude has overridden. The set is complete: all 22 pbrlib GLSL
# files that include lib/mx_closure_type.glsl.
GROUPS = [
    ('Diffuse', [
        'mx_oren_nayar_diffuse_bsdf.glsl',
        'mx_burley_diffuse_bsdf.glsl',
        'mx_translucent_bsdf.glsl',
    ]),
    ('Specular and transmissive', [
        'mx_conductor_bsdf.glsl',
        'mx_dielectric_bsdf.glsl',
        'mx_generalized_schlick_bsdf.glsl',
        'mx_sheen_bsdf.glsl',
    ]),
    ('Subsurface and hair', [
        'mx_subsurface_bsdf.glsl',
        'mx_chiang_hair_bsdf.glsl',
    ]),
    ('Emission', [
        'mx_uniform_edf.glsl',
        'mx_generalized_schlick_edf.glsl',
        'mx_add_edf.glsl',
        'mx_mix_edf.glsl',
        'mx_multiply_edf_color3.glsl',
        'mx_multiply_edf_float.glsl',
    ]),
    ('Volume', [
        'mx_anisotropic_vdf.glsl',
        'mx_layer_vdf.glsl',
    ]),
    ('Combinators', [
        'mx_mix_bsdf.glsl',
        'mx_add_bsdf.glsl',
        'mx_layer_bsdf.glsl',
        'mx_multiply_bsdf_color3.glsl',
        'mx_multiply_bsdf_float.glsl',
    ]),
]

glsl_dir = os.path.dirname(stock_impl)
CLOSURE_FILES = set()
for name in os.listdir(glsl_dir):
    if name.endswith('.glsl'):
        body = io.open(os.path.join(glsl_dir, name), encoding='utf-8', newline='').read()
        if 'lib/mx_closure_type.glsl' in body:
            CLOSURE_FILES.add(name)

text = io.open(stock_impl, encoding='utf-8', newline='').read()
decls = []
for m in re.finditer(
        r'<implementation\s+name="(?P<name>[^"]+)"\s+nodedef="(?P<nodedef>[^"]+)"'
        r'\s+file="(?P<file>[^"]+)"\s+function="(?P<function>[^"]+)"\s+target="genglsl"\s*/>',
        text):
    d = m.groupdict()
    if d['file'] in CLOSURE_FILES:
        decls.append(d)

by_file = {}
for d in decls:
    by_file.setdefault(d['file'], []).append(d)

lines = []
A = lines.append
A('<?xml version="1.0"?>')
A('<materialx version="1.39">')
A('')
A('  <!--')
A('    hdClaude genglsl_pt implementations of the MaterialX pbrlib closure nodes.')
A('')
A('    Version of record: MaterialX 1.39.3, the version inside OpenUSD 26.03.')
A('')
A('    THE OVERRIDE SET IS NOT OPTIONAL AND NOT PARTIAL.')
A('')
A('    MaterialX resolves a source file\'s #include relative to that file\'s own')
A('    directory, so an upstream pbrlib closure resolves lib/mx_closure_type.glsl')
A('    to *upstream\'s* copy. Generating one upstream closure beside one hdClaude')
A('    closure emits both and declares struct ClosureData twice.')
A('')
A('    The set is exactly the pbrlib GLSL files that include mx_closure_type.glsl:')
A('    22 files in 1.39.3, listed below. No stdlib file includes it, which is what')
A('    keeps the inheritance boundary clean.')
A('')
A('    NODEDEF NAMES DO NOT FOLLOW FILE NAMES. mx_multiply_bsdf_color3.glsl is')
A('    declared for ND_multiply_bsdfC, not ND_multiply_bsdf_color3. A declaration')
A('    naming a nodedef that does not exist is ignored SILENTLY: MaterialX uses')
A('    the stock implementation, and the only symptom is the duplicate ClosureData')
A('    above. The nodedef names here are taken verbatim from the stock')
A('    pbrlib_genglsl_impl.mtlx, and tests/materialx_tests.cpp asserts that every')
A('    one of them resolves.')
A('  -->')
A('')

# The surface node has no GLSL file: its implementation is entirely C++, and the
# declaration exists only to bind the nodedef to a target. Without it MaterialX
# resolves `surface` through target inheritance to the stock SurfaceNodeGlsl,
# which emits a rasteriser light loop and a prefiltered-environment lookup -- and
# does so silently.
A('  <!-- ' + '=' * 70 + ' -->')
A('  <!-- ' + 'Surface: the calling convention'.ljust(70) + ' -->')
A('  <!--                                                                        -->')
A('  <!-- Bodiless by design, exactly as the stock declaration is: implemented    -->')
A('  <!-- in C++ by hdclaude::PathTracerSurfaceNode, which evaluates the BSDF     -->')
A('  <!-- and EDF against the caller-supplied closureData instead of building     -->')
A('  <!-- its own inside a light loop.                                            -->')
A('  <!-- ' + '=' * 70 + ' -->')
A('')
A('  <implementation name="IM_surface_genglsl_pt"')
A('                  nodedef="ND_surface"')
A('                  target="genglsl_pt" />')
A('')

covered = set()
for title, files in GROUPS:
    A('  <!-- ' + '=' * 70 + ' -->')
    A('  <!-- ' + title.ljust(70) + ' -->')
    if title == 'Combinators':
        A('  <!--                                                                        -->')
        A('  <!-- These carry the sampling correctness of the whole target. A closure    -->')
        A('  <!-- primitive that samples slightly wrong produces slightly wrong shading; -->')
        A('  <!-- a combinator that reports the selected child density instead of the -->')
        A('  <!-- mixture density makes every MIS weight in the renderer wrong.          -->')
    A('  <!-- ' + '=' * 70 + ' -->')
    A('')
    for f in files:
        for d in by_file.get(f, []):
            covered.add(f)
            A('  <implementation name="%s_genglsl_pt"' % d['name'][:-len('_genglsl')])
            A('                  nodedef="%s"' % d['nodedef'])
            A('                  file="%s"' % d['file'])
            A('                  function="%s"' % d['function'])
            A('                  target="genglsl_pt" />')
            A('')

remaining = [f for f in sorted(by_file) if f not in covered]
A('  <!--')
A('    ' + '=' * 68)
A('    STILL TO OVERRIDE - %d files.' % len(remaining))
A('    ' + '=' * 68)
A('')
A('    Generation cannot succeed for a material using any of these until the')
A('    override exists, for the reason at the top of this file.')
A('')
for f in remaining:
    names = ', '.join(d['nodedef'] for d in by_file[f])
    A('      %-34s %s' % (f, names))
A('  -->')
A('')
A('</materialx>')

io.open(out_path, 'w', encoding='utf-8', newline='').write('\n'.join(lines) + '\n')
print('wrote %s: %d implemented, %d remaining'
      % (out_path, len(covered), len(remaining)))
