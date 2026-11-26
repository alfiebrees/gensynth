
#!/bin/bash
# Builds gen synth and packages it into a macOS installer (.pkg)
# Usage:  ./make_installer.sh
#
# Optional environment variables (only needed to share the installer with others):
#   DEV_ID_APP        "Developer ID Application: Your Name (TEAMID)"
#   DEV_ID_INSTALLER  "Developer ID Installer: Your Name (TEAMID)"
#   NOTARY_PROFILE    name of a notarytool keychain profile (xcrun notarytool store-credentials)

set -euo pipefail
cd "$(dirname "$0")"

VERSION="0.2.0"
PRODUCT="gen synth"
IDENTIFIER="com.gen.gensynth"
COMPONENT="build/GenSynth_artefacts/Release/AU/${PRODUCT}.component"

echo "==> Checking JUCE"
if [ ! -d JUCE ]; then
    git clone --depth 1 --branch 8.0.6 https://github.com/juce-framework/JUCE.git
fi

echo "==> Building (universal: arm64 + x86_64)"
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release

if [ ! -d "$COMPONENT" ]; then
    echo "ERROR: build output not found at: $COMPONENT"
    echo "Look inside build/GenSynth_artefacts/ for the .component and adjust COMPONENT in this script."
    exit 1
fi

echo "==> Signing plugin"
if [ -n "${DEV_ID_APP:-}" ]; then
    codesign --force --deep --options runtime --timestamp --sign "$DEV_ID_APP" "$COMPONENT"
else
    codesign --force --deep --sign - "$COMPONENT"   # ad-hoc (fine for your own Mac)
fi

echo "==> Staging payload"
rm -rf pkgroot dist
mkdir -p "pkgroot/Library/Audio/Plug-Ins/Components" dist
cp -R "$COMPONENT" "pkgroot/Library/Audio/Plug-Ins/Components/"

echo "==> Building component package"
pkgbuild --analyze --root pkgroot dist/components.plist >/dev/null
# make sure the installer always writes to /Library, never "updates" a copy elsewhere
/usr/libexec/PlistBuddy -c "Set :0:BundleIsRelocatable false" dist/components.plist
chmod +x installer/scripts/postinstall

pkgbuild \
    --root pkgroot \
    --component-plist dist/components.plist \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --install-location "/" \
    --scripts installer/scripts \
    dist/gen-synth-component.pkg

echo "==> Building installer"
cat > dist/distribution.xml << XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>${PRODUCT}</title>
    <welcome file="welcome.txt"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_anywhere="false" enable_currentUserHome="false" enable_localSystem="true"/>
    <choices-outline>
        <line choice="default">
            <line choice="${IDENTIFIER}"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="${IDENTIFIER}" visible="false">
        <pkg-ref id="${IDENTIFIER}"/>
    </choice>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}" onConclusion="none">gen-synth-component.pkg</pkg-ref>
</installer-gui-script>
XML

OUT="dist/${PRODUCT} ${VERSION}.pkg"
if [ -n "${DEV_ID_INSTALLER:-}" ]; then
    productbuild --distribution dist/distribution.xml --resources installer/resources \
        --package-path dist --sign "$DEV_ID_INSTALLER" "$OUT"
else
    productbuild --distribution dist/distribution.xml --resources installer/resources \
        --package-path dist "$OUT"
fi

if [ -n "${NOTARY_PROFILE:-}" ] && [ -n "${DEV_ID_INSTALLER:-}" ]; then
    echo "==> Notarizing (this can take a few minutes)"
    xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
    xcrun stapler staple "$OUT"
fi

echo ""
echo "Done:  $OUT"
