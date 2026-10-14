#!/usr/bin/env bash
# Runs one of the USD test scripts in the OpenUSD environment; ctest calls it.
# The Linux counterpart of the run_*.bat wrappers beside it.
# Usage: run_in_usd_env.sh <script.py> <output directory>
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/../../setup_usd_env.sh" || exit 1
exec python3 "$here/$1" "$2"
