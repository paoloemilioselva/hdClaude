"""usdview plugin: hdClaude > Render Stats opens a live panel of the delegate's
render stats (stats_panel.py).

usdview's own HUD prints a fixed set of Storm's stats -- hdClaude publishes
those too, so View > HUD > GPU Stats shows its memory and samples -- and has
no way to show anything else. This panel shows the rest: progress, how the
stage's instancing was flattened and what one placement costs, where the
device memory went, and where the time went (docs/render-stats.md).

Installed into <USDEXTRA>/lib/python/hdClaudeUsdview, with its plugInfo.json
in <USDEXTRA>/plugin/usd/hdClaudeUsdview/resources; setup_usd_env puts both on
the paths usdview searches. HDCLAUDE_USDVIEW_STATS=1 opens the panel when
usdview starts.
"""

from pxr import Tf
from pxr.Usdviewq.plugin import PluginContainer


def _toggle_panel(usdviewApi):
    # Imported here, so usdview's start-up does not import Qt code for it.
    from .stats_panel import toggle_panel
    toggle_panel(usdviewApi)


class HdClaudePluginContainer(PluginContainer):
    def registerPlugins(self, plugRegistry, usdviewApi):
        self._stats = plugRegistry.registerCommandPlugin(
            "HdClaudePluginContainer.renderStats", "Render Stats",
            _toggle_panel,
            "Show or hide a live panel of hdClaude's render stats")
        self._api = usdviewApi

    def configureView(self, plugRegistry, plugUIBuilder):
        plugUIBuilder.findOrCreateMenu("hdClaude").addItem(self._stats)
        from .stats_panel import open_at_startup
        open_at_startup(self._api)


Tf.Type.Define(HdClaudePluginContainer)
