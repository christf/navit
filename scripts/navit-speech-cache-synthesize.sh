#!/bin/sh
# Synthesize speech text to an Opus file.
# Usage: navit-speech-cache-synthesize.sh <text> <output.opus>
#
# Customize this script for your TTS engine.  The text is passed as
# the first argument; write the resulting audio to the file given as
# the second argument in Opus format.
#
# Examples:
#   espeak -w tmp.wav "$1" && opusenc tmp.wav "$2"
#   pico2wave -w tmp.wav "$1" && opusenc tmp.wav "$2"
#   marytts ... | opusenc - "$2"

text="$1"
output="$2"

if [ -z "$text" ] || [ -z "$output" ]; then
    echo "Usage: $0 <text> <output.opus>" >&2
    exit 1
fi

if command -v espeak >/dev/null 2>&1 && command -v opusenc >/dev/null 2>&1; then
    tmp=$(mktemp /tmp/navit-cache-XXXXXX.wav)
    espeak "$text" -w "$tmp" 2>/dev/null
    opusenc "$tmp" "$output" 2>/dev/null
    rm -f "$tmp"
elif command -v pico2wave >/dev/null 2>&1 && command -v opusenc >/dev/null 2>&1; then
    tmp=$(mktemp /tmp/navit-cache-XXXXXX.wav)
    pico2wave -w "$tmp" "$text" 2>/dev/null
    opusenc "$tmp" "$output" 2>/dev/null
    rm -f "$tmp"
else
    echo "navit-speech-cache-synthesize: no TTS engine found (install espeak + opus-tools)" >&2
    exit 1
fi
