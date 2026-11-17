"""A dockable panel of hdClaude's render stats, refreshed while it is shown.

Everything it shows comes from one `GetRenderStats()` call twice a second, on
usdview's GUI thread. The delegate answers that from figures it recorded when
it did the work -- atomics and a copied string, no device readback and no lock
the render thread holds -- so the panel costs the render nothing measurable,
and nothing at all while it is hidden: the timer runs only while the panel is
visible (docs/render-stats.md).
"""

import os

from pxr.Usdviewq.qt import QtCore, QtGui, QtWidgets

RENDERER_ID = "HdClaudeRendererPlugin"
REFRESH_MS = 500
OBJECT_NAME = "hdClaudeRenderStats"


def _engine(usdviewApi):
    """The viewport's UsdImagingGL engine, or None.

    usdview's plugin API offers no route to it, so this reads the attributes
    usdview itself uses (UsdviewApi's app controller, its stage view, and the
    view's renderer) and gives up quietly when a version names them otherwise.
    """
    controller = getattr(usdviewApi, "_UsdviewApi__appController", None)
    view = getattr(controller, "_stageView", None)
    return getattr(view, "_renderer", None)


def _bytes(value):
    value = float(value or 0)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024.0:
            return ("%.0f %s" if unit == "B" else "%.1f %s") % (value, unit)
        value /= 1024.0
    return "%.1f TB" % value


def _count(value):
    value = float(value or 0)
    if value >= 1e9:
        return "%.2f G" % (value / 1e9)
    if value >= 1e6:
        return "%.2f M" % (value / 1e6)
    return "{:,.0f}".format(value)


def _seconds(value):
    value = float(value or 0)
    if value < 1.0:
        return "%.0f ms" % (value * 1000.0)
    if value < 120.0:
        return "%.1f s" % value
    return "%dm %02ds" % (int(value) // 60, int(value) % 60)


def _ms(value):
    return _seconds(float(value or 0) / 1000.0)


def _escape(text):
    return (str(text).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;"))


def _render(stats):
    """The panel's contents for one stats dictionary, as rich text."""
    get = stats.get
    rows = []

    def section(title):
        rows.append("<tr><td colspan='2' style='padding-top:8px'>"
                    "<b>%s</b></td></tr>" % _escape(title))

    def row(label, value):
        rows.append("<tr><td style='padding-right:14px'>%s</td>"
                    "<td align='right'>%s</td></tr>"
                    % (_escape(label), _escape(value)))

    samples = int(get("samplesCompleted", 0) or 0)
    target = int(float(get("targetSamples", 0) or 0))
    percent = float(get("percentDone", 0) or 0)
    elapsed = float(get("accumulationSeconds", 0) or 0)
    section("Progress")
    row("Samples", "%d / %d" % (samples, target))
    row("Done", "%.0f%%  %s" % (percent, get("rendererStage", "")))
    row("Elapsed", _seconds(elapsed))
    if 0.0 < percent < 100.0:
        row("ETA", _seconds(elapsed * (100.0 / percent - 1.0)))
    row("Frame", _ms(get("msPerFrame", 0)))

    section("Instancing")
    row("Placements", _count(get("instances", 0)))
    row("Prototypes", _count(get("prototypes", 0)))
    row("Per prototype", "%.1f" % float(get("placementsPerPrototype", 0) or 0))
    row("Nesting depth", "%d" % int(float(get("instancingDepth", 0) or 0)))
    row("Instanced prims", _count(get("instancedRprims", 0)))
    row("Most placements", _count(get("largestPlacementCount", 0)))
    largest = get("largestPlacementPrim", "")
    if largest:
        rows.append("<tr><td colspan='2' style='color:#888'>%s</td></tr>"
                    % _escape(largest))
    row("Bytes / placement", _bytes(get("bytesPerPlacement", 0)))
    row("TLAS build", _ms(get("tlasBuildMs", 0)))
    row("TLAS chunks", "%d" % int(float(get("tlasChunks", 0) or 0)))
    row("TLAS scratch", _bytes(get("tlasScratchBytes", 0)))

    section("Device memory")
    row("Used", _bytes(get("deviceBytesUsed", 0)))
    row("Peak", _bytes(get("deviceBytesPeak", 0)))
    row("Available", _bytes(get("deviceBytesAvailable", 0)))
    memory = get("deviceMemory") or {}
    for name, value in sorted(memory.items(), key=lambda item: -float(item[1])):
        row("  " + name, _bytes(value))

    section("Time")
    for label, key in (("Ingest", "ingestMs"), ("Publish", "publishMs"),
                       ("Instancing", "instancingMs"), ("Trace", "traceMs"),
                       ("Materials", "materialMs"), ("Textures", "textureMs")):
        row(label, _ms(get(key, 0)))

    device = get("device", "")
    if device:
        rows.append("<tr><td colspan='2' style='padding-top:8px;color:#888'>"
                    "%s</td></tr>" % _escape(device))
    return "<table cellspacing='0'>%s</table>" % "".join(rows)


class StatsPanel(QtWidgets.QDockWidget):
    def __init__(self, usdviewApi, parent):
        super().__init__("hdClaude Render Stats", parent)
        self.setObjectName(OBJECT_NAME)
        self._api = usdviewApi
        self._text = ""

        self._label = QtWidgets.QLabel()
        self._label.setTextFormat(QtCore.Qt.RichText)
        self._label.setAlignment(QtCore.Qt.AlignTop | QtCore.Qt.AlignLeft)
        self._label.setTextInteractionFlags(QtCore.Qt.TextSelectableByMouse)
        font = QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont)
        self._label.setFont(font)
        self._label.setContentsMargins(8, 4, 8, 8)
        scroll = QtWidgets.QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self._label)
        self.setWidget(scroll)

        self._timer = QtCore.QTimer(self)
        self._timer.setInterval(REFRESH_MS)
        self._timer.timeout.connect(self.refresh)
        self.visibilityChanged.connect(self._visibility)

    def _visibility(self, visible):
        # Hidden, it asks for nothing.
        if visible:
            self.refresh()
            self._timer.start()
        else:
            self._timer.stop()

    def refresh(self):
        engine = _engine(self._api)
        if engine is None:
            text = "No renderer yet."
        else:
            try:
                renderer = str(engine.GetCurrentRendererId())
            except Exception:
                renderer = ""
            if renderer != RENDERER_ID:
                text = ("The viewport renderer is %s. Choose hdClaude in "
                        "View &gt; Hydra Renderer to see its stats."
                        % _escape(renderer or "not set"))
            else:
                try:
                    text = _render(dict(engine.GetRenderStats()))
                except Exception as error:  # a panel must not take usdview down
                    text = "Could not read render stats: %s" % _escape(error)
        # Set only when it changed: a converged render's stats do not move,
        # and relaying out the same text twice a second is waste.
        if text != self._text:
            self._text = text
            self._label.setText(text)


def _panel(usdviewApi, create):
    window = usdviewApi.qMainWindow
    panel = window.findChild(QtWidgets.QDockWidget, OBJECT_NAME)
    if panel is None and create:
        panel = StatsPanel(usdviewApi, window)
        window.addDockWidget(QtCore.Qt.RightDockWidgetArea, panel)
    return panel


def toggle_panel(usdviewApi):
    panel = _panel(usdviewApi, create=True)
    panel.setVisible(not panel.isVisible())
    if panel.isVisible():
        panel.raise_()


def open_at_startup(usdviewApi):
    """Show the panel at start-up when HDCLAUDE_USDVIEW_STATS asks for it."""
    value = os.environ.get("HDCLAUDE_USDVIEW_STATS", "").strip().lower()
    if value in ("1", "true", "yes", "on"):
        _panel(usdviewApi, create=True).show()
