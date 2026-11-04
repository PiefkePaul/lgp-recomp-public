#!/bin/sh
# Double-click in Finder: the LGP build assistant (tools/kit/build.py). python3 comes with the Command Line
# Tools. Without them /usr/bin/python3 only opens Apple's install dialog and ends (clean macOS VM, 2026-11-03),
# so this waits here until they are installed instead of closing the window.
cd "$(dirname "$0")" || exit 1

has_python() {
    py=$(command -v python3) || return 1
    [ "$py" != /usr/bin/python3 ] && return 0  # Homebrew's, python.org's: no Command Line Tools needed for it
    dev=$(xcode-select -p 2>/dev/null) && [ -x "$dev/usr/bin/python3" ]
}

if [ "$(uname)" = Darwin ] && ! has_python; then
    xcode-select --install >/dev/null 2>&1
    echo "Apple's developer tools (Command Line Tools) are missing; they bring Python and the compiler."
    echo "A dialog has opened: click \"Install\" and wait until it says it is done (a few minutes)."
    while ! has_python; do
        printf "Then press Enter here to go on (Ctrl+C stops). "
        read -r _ || exit 1
    done
fi
LGP_KIT_PAUSE=1 exec python3 tools/kit/build.py "$@"
