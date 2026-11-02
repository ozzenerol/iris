#!/bin/sh
# Builds iris-intellij.jar against an installed IntelliJ IDE (2026.2 or newer).
# IDE=/path/to/ide ./build.sh   (default: the Toolbox IntelliJ IDEA)
set -e
cd "$(dirname "$0")"
IDE=${IDE:-$HOME/.local/share/JetBrains/Toolbox/apps/intellij-idea}
[ -d "$IDE/lib" ] || { echo "no IDE at $IDE (set IDE=...)" >&2; exit 1; }
JAVAC=${JAVAC:-javac}
rm -rf build && mkdir -p build/classes
CP=$(find "$IDE/lib" -maxdepth 1 -name '*.jar' | tr '\n' ':')
$JAVAC --release 21 -nowarn -encoding UTF-8 -proc:none -cp "$CP" -d build/classes $(find src -name '*.java')
cp -r resources/* build/classes/
(cd build/classes && jar --create --file ../../iris-intellij.jar .)
echo "iris-intellij.jar"
