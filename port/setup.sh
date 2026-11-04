#!/bin/sh
# Fetch MacNativeGames with this project's Android changes: the android branch of our fork, at the pinned commit.
#   port/setup.sh   -> ./MacNativeGames on branch android
# After a change there: commit it on that branch, push it to the fork, and put the new commit here.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FORK=https://github.com/winterskycode/MacNativeGames.git
COMMIT=3691dcb0fdb44e99f887b800fd6d0692818a4d1d
if [ ! -d "$ROOT/MacNativeGames/.git" ]; then
  git clone -q "$FORK" "$ROOT/MacNativeGames"
  git -C "$ROOT/MacNativeGames" fetch -q origin android
  git -C "$ROOT/MacNativeGames" checkout -q -b android $COMMIT
  echo "MacNativeGames $COMMIT"
else
  echo "MacNativeGames already present"
fi
