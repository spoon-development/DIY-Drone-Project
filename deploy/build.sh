#!/usr/bin/env bash
# Moved to build/build.sh — this wrapper keeps old paths working.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../build/build.sh" "$@"
