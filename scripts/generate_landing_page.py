"""Regenerate the figures on the landing page, index.html, from their sources.

index.html is the project's landing page, served by GitHub Pages. Most of it
is written by hand -- the pipeline walkthrough, the capability list, the
standards table -- and is kept current by whoever changes what it describes,
in the same commit. Its numbers are not written by hand. Each lives between
a pair of markers

    <!-- BEGIN GENERATED name --> ... <!-- END GENERATED name -->

and is rewritten here from the record it summarises:

    version   the project version, from CMakeLists.txt's project()
    asof-*    the newest date in the roadmap's decision log and gallery.md's
              timing table
    figures   the Cornell box's worst error (gallery.md), the closure count
              (the roadmap's phase 4 evidence), the gallery scene count
              (gallery.md's timing table) and the test count
              (tests/CMakeLists.txt)
    cornell   the Cornell box comparison table, from gallery.md
    gallery   the gallery strip: each scene's wall time from gallery.md, its
              image the committed baseline itself
    phases    the phase grid, from the roadmap's phase tracker

A source that no longer reads as expected is an error, never a silent
default: the page would otherwise go on stating a figure nothing supports.

Usage:
    python scripts/generate_landing_page.py           rewrite index.html
    python scripts/generate_landing_page.py --check   fail if it is stale

ctest runs the check as usd_landing_page, and render_gallery.ps1 regenerates
the page whenever it rewrites gallery.md's timing table.
"""

import datetime
import html
import os
import re
import struct
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
PAGE = os.path.join(ROOT, "index.html")
GALLERY_MD = os.path.join(ROOT, "gallery.md")
ROADMAP_MD = os.path.join(ROOT, "docs", "roadmap.md")
TESTS_CMAKE = os.path.join(ROOT, "tests", "CMakeLists.txt")
ROOT_CMAKE = os.path.join(ROOT, "CMakeLists.txt")

# The gallery strip: which scenes, in what order, and what the caption says
# besides the wall time. The title is the one gallery.md's timing table uses.
# A scene listed here that the table does not have is an error.
GALLERY = [
    # css class, table title, image, caption note, alt text
    ("g-a", "Cornell Box", "cornell_box.jpg", "measured spectra",
     "The Cornell box, lit warm orange by its measured ceiling light, with red "
     "and green side walls and two white blocks"),
    ("g-b", "StandardShaderBall Glass", "shader_ball_glass.jpg", "",
     "A glass MaterialX shader ball holding the highlights of five rect lights"),
    ("g-c", "StandardShaderBall Gold", "shader_ball_gold.jpg", "",
     "A gold MaterialX shader ball reflecting its studio lights"),
    ("g-d", "Pixar's KitchenSet", "pixar_kitchen.jpg", "Z-up stage, instanced props",
     "Pixar's Kitchen Set interior with cabinets, table and clutter"),
    ("g-e", "OpenChessSet", "chess_board.jpg", "lit by its dome alone",
     "The OpenChessSet board and pieces lit by an HDR dome"),
    ("g-f", "OpenPBR Playground", "openpbr_playground.jpg", "",
     "The OpenPBR playground scene with many material variations"),
    ("g-g", "Collective Project 001", "collectiveproject001.jpg", "UsdLux spot shaping",
     "Collective Project 001 interior lit by a shaped spot light"),
    ("g-h", "Cull Volumes", "cull_volumes.jpg", "per-ray-kind culling",
     "Cull volumes cutting a column into rings and slicing it on a slant"),
]

STATE_CLASS = {
    "Complete": "ok",
    "In progress": "wip",
    "Not started": "ns",
    "Deferred": "def",
    "Blocked": "blk",
}


# Regions that sit inside a line of text; every other region is a block whose
# markers are on lines of their own.
INLINE_REGIONS = {"version", "asof-header", "asof-footer", "cornell-note"}


class SourceError(Exception):
    pass


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def table_rows(block):
    """The cells of each body row of the first markdown table in block."""
    rows = []
    for line in block.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            if rows:
                break
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if all(re.fullmatch(r":?-+:?", c) for c in cells):
            continue
        rows.append(cells)
    return rows[1:]  # without the header


def section(text, heading_pattern):
    m = re.search(heading_pattern, text, re.M)
    if not m:
        raise SourceError("no section matching %r" % heading_pattern)
    level = len(re.match(r"#+", m.group(0)).group(0))
    rest = text[m.end():]
    end = re.search(r"^#{1,%d} " % level, rest, re.M)
    return rest[: end.start()] if end else rest


def inline_markdown(s):
    """Code spans and strike-through, escaped; nothing else is expected."""
    s = s.replace("~~", "")
    parts = re.split(r"(`[^`]+`)", s)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("<code>%s</code>" % html.escape(p[1:-1]))
        else:
            out.append(html.escape(p))
    return "".join(out)


def jpeg_size(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"\xff\xd8":
        raise SourceError("%s is not a JPEG" % path)
    i = 2
    while i < len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h, w = struct.unpack(">HH", data[i + 5 : i + 9])
            return w, h
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        (length,) = struct.unpack(">H", data[i + 2 : i + 4])
        i += 2 + length
    raise SourceError("%s has no frame header" % path)


def long_date(iso):
    d = datetime.date.fromisoformat(iso)
    return "%d %s %d" % (d.day, d.strftime("%B"), d.year)


def wall_time(cell):
    """The wall time for a caption, or None for a scene not measured yet."""
    if cell == "Not measured":
        return None
    m = re.match(r"([\d.]+) s\b", cell)
    if not m:
        raise SourceError("unreadable wall time %r in gallery.md" % cell)
    seconds = float(m.group(1))
    if seconds < 60:
        return "%.1f s" % seconds
    minutes, rem = divmod(int(round(seconds)), 60)
    return "%d min %d s" % (minutes, rem)


# --------------------------------------------------------------------------
# Sources


def gallery_timings(gallery_md):
    m = re.search(r"<!-- gallery-timings:start -->(.*?)<!-- gallery-timings:end -->",
                  gallery_md, re.S)
    if not m:
        raise SourceError("gallery.md has no timing-table markers")
    rows = table_rows(m.group(1))
    if not rows:
        raise SourceError("gallery.md's timing table has no rows")
    timings = {}
    for cells in rows:
        if len(cells) < 7:
            raise SourceError("gallery.md timing row %r has %d cells" % (cells[0], len(cells)))
        timings[cells[0]] = {"date": cells[1], "wall": cells[2], "settings": cells[6]}
    return timings


def cornell_table(gallery_md):
    body = section(gallery_md, r"^### Cornell Box\s*$")
    m = re.search(r"^\| Window \| Reference.*$", body, re.M)
    if not m:
        raise SourceError("gallery.md's Cornell Box section has no comparison table")
    rows = table_rows(body[m.start():])
    out = []
    for cells in rows:
        if len(cells) != 4 or not cells[3].endswith("%"):
            raise SourceError("unreadable Cornell comparison row %r" % (cells,))
        out.append((cells[0], cells[1], cells[2], float(cells[3].rstrip("%"))))
    if not out:
        raise SourceError("the Cornell comparison table is empty")
    gate = re.search(r"The gate is ([\d.]+)%", body)
    control = re.search(r"misses by up to ([\d.]+)%", body)
    if not gate or not control:
        raise SourceError("gallery.md's Cornell section no longer states its gate and control")
    return out, gate.group(1), control.group(1)


def phases(roadmap_md):
    rows = table_rows(section(roadmap_md, r"^## Phase tracker\s*$"))
    out = []
    for cells in rows:
        if len(cells) < 3 or not re.fullmatch(r"\d+", cells[0]):
            raise SourceError("unreadable phase tracker row %r" % (cells[:3],))
        number, name, state = int(cells[0]), cells[1], cells[2]
        if state not in STATE_CLASS:
            raise SourceError("phase %d has state %r, not one the tracker allows" % (number, state))
        short = name.split(":", 1)[0] if ":" in name.replace("~~", "") else name
        out.append((number, short, state, cells[3] if len(cells) > 3 else ""))
    out.sort(key=lambda p: p[0])
    return out


def closure_count(phase_rows):
    for number, _, _, evidence in phase_rows:
        if number == 4:
            m = re.search(r"all (\d+) overrides", evidence)
            if m:
                return int(m.group(1))
    raise SourceError("the roadmap's phase 4 evidence no longer states 'all N overrides'")


def decision_dates(roadmap_md):
    rows = table_rows(section(roadmap_md, r"^## Decision log\s*$"))
    dates = [c[0] for c in rows if re.fullmatch(r"\d{4}-\d\d-\d\d", c[0])]
    if not dates:
        raise SourceError("the roadmap's decision log has no dated rows")
    return dates


def project_version(cmake):
    m = re.search(r"^project\(hdClaude VERSION (\d+\.\d+\.\d+)\b", cmake, re.M)
    if not m:
        raise SourceError("CMakeLists.txt no longer declares project(hdClaude VERSION x.y.z)")
    return m.group(1)


def test_count(cmake):
    literal = set(re.findall(r"add_test\(NAME ([A-Za-z0-9_]+)", cmake))
    scripted = set(re.findall(r'"(usd_[A-Za-z0-9_]+)\|[A-Za-z0-9_]+"', cmake))
    if not literal or not scripted:
        raise SourceError("tests/CMakeLists.txt no longer declares its tests as expected")
    return len(literal | scripted)


# --------------------------------------------------------------------------
# Regions


def regions():
    gallery_md = read(GALLERY_MD)
    roadmap_md = read(ROADMAP_MD)
    timings = gallery_timings(gallery_md)
    cornell, gate, control = cornell_table(gallery_md)
    phase_rows = phases(roadmap_md)

    dates = decision_dates(roadmap_md) + [
        t["date"] for t in timings.values() if re.fullmatch(r"\d{4}-\d\d-\d\d", t["date"])]
    asof = long_date(max(dates))

    worst = max(r[3] for r in cornell)
    figures = "\n".join([
        '<div class="fig"><span class="v">%.2f<small>%%</small></span><span class="l">worst error '
        'against an independent radiosity solution of the measured Cornell box</span></div>' % worst,
        '<div class="fig"><span class="v">%d</span><span class="l">MaterialX closure nodes with '
        'importance sampling, validated for energy on the GPU</span></div>' % closure_count(phase_rows),
        '<div class="fig"><span class="v">%d</span><span class="l">gallery scenes rendered, timed '
        'and gated against baselines after every change</span></div>' % len(timings),
        '<div class="fig"><span class="v">%d</span><span class="l">test suites, from unit tests '
        'to end-to-end USD renders</span></div>' % test_count(read(TESTS_CMAKE)),
    ])

    scale = 40.0 / worst
    cornell_rows = []
    for window, ref, render, off in cornell:
        cornell_rows.append(
            '<tr><td>%s</td><td class="num mono">%s</td><td class="num mono">%s</td>'
            '<td class="num">%.2f%%<span class="bar" style="width:%dpx"></span></td></tr>'
            % (html.escape(window), html.escape(ref), html.escape(render), off,
               max(2, round(off * scale))))
    cornell_html = "\n".join(cornell_rows)
    cornell_note = (
        '"Off by" is the worst channel carrying at least a tenth of the window\'s brightest. '
        'The pass threshold is %s%%. As a control, the same box with its paints reduced to RGB '
        'colours misses by up to %s%%, which shows the comparison can tell whether the measured '
        'spectra were used.' % (gate, control))

    figures_html = []
    for css, title, image, note, alt in GALLERY:
        if title not in timings:
            raise SourceError("gallery scene %r is not in gallery.md's timing table" % title)
        path = os.path.join(ROOT, "gallery", image)
        if not os.path.exists(path):
            raise SourceError("gallery image %s does not exist" % image)
        w, h = jpeg_size(path)
        caption = [html.escape(n) for n in (note, wall_time(timings[title]["wall"])) if n]
        figures_html.append(
            '<figure class="%s"><img alt="%s" src="gallery/%s" width="%d" height="%d" '
            'loading="lazy" decoding="async"><figcaption><b>%s</b> · %s</figcaption></figure>'
            % (css, html.escape(alt), image, w, h, html.escape(title), " · ".join(caption)))

    phase_html = []
    for number, name, state, _ in phase_rows:
        label = "Phase %d" % number
        if state == "Deferred":
            label += " · deferred"
        elif state == "Blocked":
            label += " · blocked"
        phase_html.append(
            '<div class="ph %s"><span class="no">%s</span><span class="t">%s</span></div>'
            % (STATE_CLASS[state], label, inline_markdown(name)))

    return {
        "version": project_version(read(ROOT_CMAKE)),
        "asof-header": asof,
        "asof-footer": asof,
        "figures": figures,
        "cornell": cornell_html,
        "cornell-note": cornell_note,
        "gallery": "\n".join(figures_html),
        "phases": "\n".join(phase_html),
    }


def apply(page, values):
    for name, value in values.items():
        pattern = re.compile(
            r"(<!-- BEGIN GENERATED %s -->)(.*?)(<!-- END GENERATED %s -->)"
            % (re.escape(name), re.escape(name)), re.S)
        matches = pattern.findall(page)
        if len(matches) != 1:
            raise SourceError("index.html has %d '%s' regions, not one" % (len(matches), name))
        body = value if name in INLINE_REGIONS else "\n" + value + "\n"
        page = pattern.sub(lambda m: m.group(1) + body + m.group(3), page)
    return page


def main(argv):
    check = "--check" in argv[1:]
    try:
        page = read(PAGE)
        updated = apply(page, regions())
    except SourceError as e:
        print("generate_landing_page: %s" % e, file=sys.stderr)
        return 1
    if updated == page:
        print("index.html is current")
        return 0
    if check:
        print("index.html is stale: run python scripts/generate_landing_page.py",
              file=sys.stderr)
        return 1
    with open(PAGE, "w", encoding="utf-8", newline="") as f:
        f.write(updated)
    print("index.html regenerated")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
