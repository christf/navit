#!/bin/sh
# Play a synthesized speech file (Opus).
# Usage: navit-speech-cache-play.sh <file>
#
# Customize this script for your system.  Common player commands:
#   paplay file.opus          (PulseAudio)
#   ffplay -nodisp -autoexit  (FFmpeg)
#   opusdec file.opus - | aplay

file="$1"
if [ -z "$file" ] || [ ! -f "$file" ]; then
    exit 1
fi

tmp=""
cleanup() { kill -- "$child" 2>/dev/null; wait "$child" 2>/dev/null; rm -f "$tmp"; }
trap cleanup TERM INT

if command -v paplay >/dev/null 2>&1; then
    paplay "$file" &
elif command -v ffplay >/dev/null 2>&1; then
    ffplay -nodisp -autoexit "$file" 2>/dev/null &
elif command -v aplay >/dev/null 2>&1 && command -v opusdec >/dev/null 2>&1; then
    tmp=$(mktemp /tmp/navit-cache-XXXXXX.wav)
    opusdec "$file" "$tmp" 2>/dev/null && aplay "$tmp" &
else
    echo "navit-speech-cache-play: no suitable player found" >&2
    exit 1
fi

child=$!
wait "$child"
ret=$?
rm -f "$tmp"
exit "$ret"
