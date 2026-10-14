#!/bin/sh
# Run tests/crash_check.cpp under Wine; the reports go to a log in a temp folder, printed at the end.
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
log="$dir/crash-check.log"
WINEPREFIX=${WINEPREFIX:-$HOME/.wine-prk} WINEDEBUG=-all RANDYVK_LOG="$(winepath -w "$log")" RANDYVK_HANG_SECONDS=5 \
    timeout 60 wine build/linux-release/crash_check.exe "$@" 2>&1 | grep -v -iE 'pci id|EGL'
echo "--- $log"
cat "$log"
