#!/usr/bin/env bash
# Fail the docs build if Sphinx reports any warnings.
# With warnings cleaned up, a simple check is sufficient.

set -uo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_root="$(mktemp -d)"
trap 'rm -rf "$build_root"' EXIT

cd "$root"

log="$build_root/build.log"
sphinx-build -q --keep-going -b html docs "$build_root/html" 2> "$log"
code=$?
if [ $code -ne 0 ]; then
    echo "sphinx-build failed with exit code $code." >&2
    cat "$log" >&2
    exit $code
fi

if grep -q ": WARNING:" "$log"; then
    echo "Sphinx reported warnings:" >&2
    grep ": WARNING:" "$log" >&2
    exit 1
fi

echo "Documentation builds without warnings."
exit 0
