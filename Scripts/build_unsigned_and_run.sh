#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Compatibility entry point. Interactive development always runs the stable,
# locally signed app so rebuilds retain the same macOS TCC identity.
exec "$SCRIPT_DIR/build_stable_dev_and_run.sh" "$@"
