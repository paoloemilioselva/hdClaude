#!/usr/bin/env bash
# Render with the hdClaude delegate. Accepts normal usdrecord arguments.
#
# The Linux counterpart of render_claude.bat, which explains each fixed flag
# below: camera lighting off so authored lighting is what gets tested, the
# render purpose always, and colour correction off so the output stays
# scene-linear. Those reasons hold on every platform; change both or neither.

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/setup_usd_env.sh" || exit $?

if [ $# -eq 0 ]; then
  # The settings listing lives once, in render_claude.bat.
  sed -n -e 's/\r$//' -e 's/^  ECHO\.$//p' -e 's/^  ECHO \{0,1\}//p' "$here/render_claude.bat" |
    sed -e 's/\^\(.\)/\1/g' -e 's/render_claude\.bat/render_claude.sh/'
  exit 2
fi

exec usdrecord --renderer "Claude GPU Path Tracer" --disableCameraLight \
     --colorCorrectionMode disabled --purposes render "$@"
