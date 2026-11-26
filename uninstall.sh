#!/bin/bash
# Removes gen synth (leaves your samples folder alone)
sudo rm -rf "/Library/Audio/Plug-Ins/Components/gen synth.component"
rm -rf "$HOME/Library/Audio/Plug-Ins/Components/gen synth.component"
sudo pkgutil --forget com.gen.gensynth 2>/dev/null || true
killall -9 AudioComponentRegistrar 2>/dev/null || true
echo "gen synth removed."
