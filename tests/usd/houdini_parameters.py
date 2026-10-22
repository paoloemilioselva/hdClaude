"""The Houdini parameter files describe the delegate's current settings.

Runs scripts/generate_houdini_parameters.py --check, which fails when a
setting was added, renamed or re-defaulted without regenerating the files
Houdini reads -- or when a new setting was never given a folder.

Usage: python houdini_parameters.py <output directory, unused>
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "..", "scripts", "generate_houdini_parameters.py")

sys.exit(subprocess.call([sys.executable, SCRIPT, "--check"]))
