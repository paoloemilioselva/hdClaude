"""Report how a stage's materials and shaders are plugged together, at the USD level.

Usage:
    python tools/check_usd_materials.py <stage.usd> [<stage.usd> ...]
    python tools/check_usd_materials.py --quiet <stage.usd>     errors only

Exit status is 1 if any ERROR was reported, so this can gate a scene before it
reaches the renderer.

This tool reports; it never repairs. hdClaude does not coerce an input into the
type a shader wanted -- a renderer that silently patches malformed input hides
the defect from the person who can fix it, and makes its own image
untrustworthy, because you can no longer tell which pixels came from the asset
and which from the workaround. What is wrong in a scene gets fixed in the scene.

The checks are deliberately USD-only. They read the authored attribute types and
connections and nothing else, so a finding here is a statement about the asset
that holds no matter which renderer, Hydra delegate, or MaterialX version
consumes it. That boundary is the point of the tool: a mismatch that this script
does *not* report, but which appears once the network has been translated to
MaterialX, is by elimination a bug in the translation rather than in the scene.

The type rule, which is the subtle one
--------------------------------------
USD distinguishes a value's *type* from its *role*. `normal3f`, `vector3f`,
`point3f`, `color3f` and `float3` are five different `SdfValueTypeName`s that
all carry a `GfVec3f`; the role says what the three numbers mean, not how many
there are. Connecting `float3` to `normal3f` is therefore well-formed USD, and
is exactly what the UsdPreviewSurface specification prescribes for a normal map:
a `UsdUVTexture` declares `float3 outputs:rgb`, and `inputs:normal` is
`normal3f`.

So a differing role is reported as INFO at most. What is a real error is a
differing *type*: connecting a three-component output to a one-component input
loses two of them, and USD has no rule that says which one survives. That case
is common in exported assets -- a scalar roughness or metalness input wired to a
texture's `rgb` instead of its `r` -- and it is a genuine authoring bug with a
one-line fix in the scene.
"""
import os
import sys

from pxr import Sdf, Usd, UsdShade

ERROR, WARNING, INFO = "ERROR", "WARNING", "INFO"


class Report:
    """Findings for one stage, grouped so a prim's problems read together."""

    def __init__(self, identifier):
        self.identifier = identifier
        self.findings = []

    def add(self, severity, path, message):
        self.findings.append((severity, str(path), message))

    def count(self, severity):
        return sum(1 for f in self.findings if f[0] == severity)

    def write(self, out, quiet=False):
        shown = [f for f in self.findings if not (quiet and f[0] != ERROR)]
        out.write("\n%s\n" % self.identifier)
        out.write("%s\n" % ("-" * len(self.identifier)))
        if not shown:
            out.write("  nothing to report\n")
        else:
            by_path = {}
            for severity, path, message in shown:
                by_path.setdefault(path, []).append((severity, message))
            for path in sorted(by_path):
                out.write("  <%s>\n" % path)
                for severity, message in by_path[path]:
                    out.write("    %-7s %s\n" % (severity, message))
        out.write("  %d error(s), %d warning(s), %d note(s)\n" %
                  (self.count(ERROR), self.count(WARNING), self.count(INFO)))


def _dimension(value_type):
    """How many components a value type carries, or None if that is not a count.

    The comparison that matters is component count, not the type name: dropping
    two of three components is a loss the scene has to resolve, whereas the same
    three numbers under a different role is not.
    """
    if value_type is None:
        return None
    scalar = value_type.scalarType if value_type.isArray else value_type
    default = scalar.defaultValue
    try:
        return len(default)
    except TypeError:
        return 1


def _source_of(attribute):
    """The single connection authored on `attribute`, or None.

    Returns (target_path, extra_findings). More than one source on a shader
    input is not something USD's shading model defines, so it is reported.
    """
    targets = attribute.GetConnections()
    if not targets:
        return None, []
    if len(targets) > 1:
        return targets[0], [(
            WARNING,
            "input '%s' authors %d connections; USD shading uses one source, "
            "so the others are ignored" % (attribute.GetName(), len(targets)))]
    return targets[0], []


def _check_connection(stage, prim, attribute, report):
    """Resolve one authored connection and check that it is well formed."""
    target, extra = _source_of(attribute)
    for severity, message in extra:
        report.add(severity, prim.GetPath(), message)
    if target is None:
        return

    name = attribute.GetName()
    source_prim = stage.GetPrimAtPath(target.GetPrimPath())
    if not source_prim or not source_prim.IsValid():
        report.add(ERROR, prim.GetPath(),
                   "input '%s' is connected to <%s>, whose prim does not exist "
                   "on this stage" % (name, target))
        return

    source_attribute = source_prim.GetAttribute(target.name)
    if not source_attribute or not source_attribute.IsValid():
        # An output that is merely undeclared still resolves at render time for
        # some schemas, but a name that does not exist at all is a dead link,
        # and the two are indistinguishable from the scene alone. Reported as
        # an error because nothing downstream can supply the value.
        report.add(ERROR, prim.GetPath(),
                   "input '%s' is connected to <%s>, which the source prim "
                   "does not author" % (name, target))
        return

    destination_type = attribute.GetTypeName()
    source_type = source_attribute.GetTypeName()
    if destination_type == source_type:
        return

    destination_size = _dimension(destination_type)
    source_size = _dimension(source_type)
    if destination_size != source_size:
        report.add(ERROR, prim.GetPath(),
                   "input '%s' is %s (%s component(s)) but is connected to "
                   "<%s>, which is %s (%s component(s)); connect a matching "
                   "output instead" %
                   (name, destination_type, destination_size, target,
                    source_type, source_size))
    elif destination_type.type != source_type.type:
        report.add(ERROR, prim.GetPath(),
                   "input '%s' is %s but is connected to <%s>, which is %s; "
                   "these carry different value types" %
                   (name, destination_type, target, source_type))
    else:
        # Same components, same C++ type, different role. Legal USD -- and the
        # normal-map wiring the UsdPreviewSurface spec asks for.
        report.add(INFO, prim.GetPath(),
                   "input '%s' is %s and is connected to <%s>, which is %s; "
                   "the roles differ but the value type does not, which USD "
                   "allows" % (name, destination_type, target, source_type))


def _anchor_directory(attribute):
    """The directory an unresolved relative asset path is relative to."""
    stack = attribute.GetPropertyStack(Usd.TimeCode.Default())
    for spec in stack:
        real = spec.layer.realPath
        if real:
            return os.path.dirname(real)
    return None


def _check_udim_set(prim, attribute, value, report):
    """A `<UDIM>` path is a tile pattern, so at least one tile must exist.

    USD deliberately does not resolve the pattern itself -- `<UDIM>` is not a
    filename -- so asking the resolver about it and reporting the empty answer
    would fail every correctly authored tile set. What can be checked is the
    thing that actually matters: whether any tile is on disk. The numbering is
    `1001 + u + 10*v`, and the first row and column is where essentially all
    real sets live, so a miss across 1001-1100 means the set is not there.
    """
    directory = _anchor_directory(attribute)
    if directory is None:
        return
    for tile in range(1001, 1101):
        candidate = value.path.replace("<UDIM>", str(tile))
        if os.path.isabs(candidate):
            resolved = candidate
        else:
            resolved = os.path.normpath(os.path.join(directory, candidate))
        if os.path.exists(resolved):
            return
    report.add(ERROR, prim.GetPath(),
               "input '%s' names the UDIM set @%s@, but no tile in 1001-1100 "
               "exists on disk" % (attribute.GetName(), value.path))


def _check_assets(prim, attribute, report):
    """An asset-valued input whose path does not resolve is a missing texture."""
    if attribute.GetTypeName() != Sdf.ValueTypeNames.Asset:
        return
    value = attribute.Get()
    if value is None or not value.path:
        return

    if "<UDIM>" in value.path or "<UVTILE>" in value.path:
        _check_udim_set(prim, attribute, value, report)
        return

    if not value.resolvedPath:
        report.add(ERROR, prim.GetPath(),
                   "input '%s' names @%s@, which does not resolve to a file" %
                   (attribute.GetName(), value.path))


def _check_shader(stage, prim, report):
    shader = UsdShade.Shader(prim)
    identifier = shader.GetShaderId() if shader else ""
    if not identifier:
        report.add(WARNING, prim.GetPath(),
                   "shader authors no info:id, so no renderer can know what to "
                   "evaluate for it")

    for attribute in prim.GetAttributes():
        name = attribute.GetName()
        if not name.startswith("inputs:"):
            continue
        _check_assets(prim, attribute, report)
        if attribute.HasAuthoredConnections():
            _check_connection(stage, prim, attribute, report)


TERMINALS = ("surface", "displacement", "volume")


def _is_terminal(attribute_name):
    """Whether `outputs:...` names a material terminal, in any render context.

    A material may bind its terminal for a specific render context:
    `outputs:mtlx:surface` is what a MaterialX-authored asset writes, and the
    Open Chess Set writes exactly that for all 41 of its materials. Only the
    *last* component names the terminal; whatever sits between `outputs:` and it
    is the render context, which a scene-level check has no business
    restricting. Looking only at the universal `outputs:surface` reports a
    correctly wired MaterialX asset as shading nothing.
    """
    if not attribute_name.startswith("outputs:"):
        return False
    return attribute_name.rsplit(":", 1)[-1] in TERMINALS


def _check_material(stage, prim, report):
    connected = [a for a in prim.GetAttributes()
                 if _is_terminal(a.GetName()) and a.HasAuthoredConnections()]
    if not connected:
        report.add(ERROR, prim.GetPath(),
                   "material connects none of its surface, displacement or "
                   "volume outputs in any render context, so it shades nothing")

    for attribute in prim.GetAttributes():
        if attribute.GetName().startswith("outputs:") and \
                attribute.HasAuthoredConnections():
            _check_connection(stage, prim, attribute, report)


def _all_prims(stage):
    """Every prim whose materials matter, instance prototypes included.

    `Stage.TraverseAll` does not descend into an instance: the contents of an
    instanceable prim live in a prototype, and the instance itself has no
    children. Pixar's Kitchen Set is instanced throughout, so traversing the
    stage alone finds 453 prims -- every one of them an Xform -- and reports the
    scene as authoring no materials at all. That is not merely incomplete; it is
    the kind of wrong that reads as a clean bill of health.

    Prototypes are walked once each rather than once per instance. A material
    inside a prototype is one authored material however many times it is placed,
    and reporting it once is what keeps the output actionable.
    """
    for prim in stage.TraverseAll():
        yield prim
    for prototype in stage.GetPrototypes():
        for prim in Usd.PrimRange(prototype):
            yield prim


def check_stage(identifier):
    report = Report(identifier)
    stage = Usd.Stage.Open(identifier)
    if not stage:
        report.add(ERROR, "/", "could not open the stage")
        return report

    materials = 0
    shaders = 0
    for prim in _all_prims(stage):
        if prim.IsA(UsdShade.Material):
            materials += 1
            _check_material(stage, prim, report)
        elif prim.IsA(UsdShade.Shader):
            shaders += 1
            _check_shader(stage, prim, report)
        elif prim.IsA(UsdShade.NodeGraph):
            # A nodegraph's own outputs are connections too, and a broken one
            # breaks every material that reaches through it.
            for attribute in prim.GetAttributes():
                if attribute.GetName().startswith("outputs:") and \
                        attribute.HasAuthoredConnections():
                    _check_connection(stage, prim, attribute, report)

    if materials == 0:
        report.add(WARNING, "/", "the stage authors no UsdShade materials")
    report.identifier = "%s  (%d material(s), %d shader(s), %d prototype(s))" % (
        identifier, materials, shaders, len(stage.GetPrototypes()))
    return report


def main(argv):
    quiet = "--quiet" in argv
    stages = [a for a in argv[1:] if not a.startswith("--")]
    if not stages:
        sys.stderr.write(__doc__.split("\n\n")[0] + "\n\n")
        sys.stderr.write(
            "Usage: python tools/check_usd_materials.py <stage.usd> [...]\n")
        return 2

    errors = 0
    for identifier in stages:
        report = check_stage(identifier)
        report.write(sys.stdout, quiet=quiet)
        errors += report.count(ERROR)

    sys.stdout.write("\n%d error(s) across %d stage(s)\n" %
                     (errors, len(stages)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
