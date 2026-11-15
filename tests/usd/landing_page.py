"""The landing page's figures match the records they summarise.

Runs scripts/generate_landing_page.py --check, which fails when gallery.md,
the roadmap or the test list changed without index.html being regenerated --
or when one of them no longer reads the way the generator expects.

Usage: python landing_page.py <output directory, unused>
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "scripts", "generate_landing_page.py")

sys.exit(subprocess.call([sys.executable, SCRIPT, "--check"]))
