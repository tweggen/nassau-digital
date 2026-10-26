#!/usr/bin/env bash
#
# ci/install.sh - a no-op, and it says so.
#
# Every Nassau repo carries the four verbs (build, clean, install,
# uninstall). This one is a library with nothing to install: qbx builds the
# core from source as a git submodule (DESIGN §9.2), and the later plugin
# player links it the same way. So there is no artefact to copy anywhere,
# and this script changes nothing on the machine.
set -euo pipefail
echo "ci/install.sh: nothing to install - nassau-digital is a library that qbx builds from source as a submodule (DESIGN §9.2). Nothing was changed."
