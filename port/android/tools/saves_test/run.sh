#!/bin/bash
# Saves.java on this PC, on small fake saves (no device, no game data): port/android/tools/saves_test/run.sh [work folder]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=${1:-$(mktemp -d)}
mkdir -p "$work/classes"
javac -Xlint:all -Xlint:-options -source 11 -target 11 -d "$work/classes" "$here/../../app/java/dev/tok/totk/Saves.java" "$here/SavesTest.java"
java -cp "$work/classes" dev.tok.totk.SavesTest "$work"
