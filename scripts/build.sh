#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/.build"
if [[ $# -gt 0 ]]; then
    if [[ $# -ne 2 || "$1" != --output || "$2" != /* ]]; then
        echo 'Usage: bash scripts/build.sh [--output ABSOLUTE_DIRECTORY]' >&2; exit 2
    fi
    OUT="$2"
    if [[ -e "$OUT" || -L "$OUT" ]]; then
        echo 'Explicit build output must be a new directory; existing paths are preserved.' >&2; exit 2
    fi
fi
SWIFT_TARGET="$(uname -m)-apple-macos26.0"
mkdir -p "$OUT"
SPARKLE=$(bash "$ROOT/scripts/prepare-sparkle.sh")
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/session_test.cpp" "$ROOT/core/session.cpp" -o "$OUT/session-test"
"$OUT/session-test"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -I"$ROOT/core" "$ROOT/tests/trigger_core_test.cpp" "$ROOT/core/session.cpp" -o "$OUT/trigger-core-test"
"$OUT/trigger-core-test"
xcrun clang -mmacosx-version-min=26.0 -std=c17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/third_party/yyjson" -c "$ROOT/third_party/yyjson/yyjson.c" -o "$OUT/yyjson.o"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -c "$ROOT/core/session.cpp" -o "$OUT/session.o"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" -c "$ROOT/core/protocol.cpp" -o "$OUT/protocol.o"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" -c "$ROOT/core/workflow.cpp" -o "$OUT/workflow.o"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" -c "$ROOT/core/workflow_bridge.cpp" -o "$OUT/workflow_bridge.o"
CORE_OBJECTS=("$OUT/session.o" "$OUT/protocol.o" "$OUT/yyjson.o")
WORKFLOW_OBJECTS=("${CORE_OBJECTS[@]}" "$OUT/workflow.o" "$OUT/workflow_bridge.o")
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" "$ROOT/tests/protocol_test.cpp" "${CORE_OBJECTS[@]}" -o "$OUT/protocol-test"
"$OUT/protocol-test"
xcrun clang++ -mmacosx-version-min=26.0 -std=c++17 -Wall -Wextra -Werror -DYYJSON_READER_DEPTH_LIMIT=513 -I"$ROOT/core" -I"$ROOT/third_party/yyjson" "$ROOT/tests/workflow_core_test.cpp" "${WORKFLOW_OBJECTS[@]}" -o "$OUT/workflow-core-test"
"$OUT/workflow-core-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/session.h" "$ROOT/tests/core_bridge_test.swift" "$OUT/session.o" -lc++ -o "$OUT/core-bridge-test"
"$OUT/core-bridge-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/tests/protocol_bridge_test.swift" "${CORE_OBJECTS[@]}" -lc++ -o "$OUT/protocol-bridge-test"
"$OUT/protocol-bridge-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/tests/diagnostics_test.swift" "${CORE_OBJECTS[@]}" -lc++ -o "$OUT/diagnostics-test"
"$OUT/diagnostics-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/session.h" "$ROOT/macos/InsertionPolicy.swift" "$ROOT/tests/insertion_policy_test.swift" "$OUT/session.o" -lc++ -o "$OUT/policy-test"
"$OUT/policy-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/session.h" "$ROOT/macos/TriggerChoice.swift" "$ROOT/macos/HotKey.swift" "$ROOT/tests/hotkey_test.swift" "$OUT/session.o" -lc++ -framework AppKit -framework Carbon -o "$OUT/hotkey-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors "$ROOT/macos/IndicatorPanel.swift" "$ROOT/tests/overlay_test.swift" -framework AppKit -o "$OUT/overlay-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/TransientAudio.swift" "$ROOT/tests/pipeline_test.swift" "${CORE_OBJECTS[@]}" -lc++ -o "$OUT/pipeline-test"
"$OUT/pipeline-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/TransientAudio.swift" "$ROOT/macos/Recorder.swift" "$ROOT/tests/recorder_test.swift" "${CORE_OBJECTS[@]}" -lc++ -framework AVFoundation -o "$OUT/recorder-test"
"$OUT/recorder-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/workflow_bridge.h" "$ROOT/macos/WorkflowBridge.swift" "$ROOT/tests/workflow_bridge_test.swift" "${WORKFLOW_OBJECTS[@]}" -lc++ -o "$OUT/workflow-bridge-test"
"$OUT/workflow-bridge-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/workflow_bridge.h" "$ROOT/macos/WorkflowLaunchTracker.swift" "$ROOT/tests/workflow_controller_test.swift" -o "$OUT/workflow-controller-test"
"$OUT/workflow-controller-test"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -swift-version 5 -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/workflow_bridge.h" "$ROOT/macos/WorkflowBridge.swift" "$ROOT/macos/WorkflowNativeAdapter.swift" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/TransientAudio.swift" "$ROOT/macos/Recorder.swift" "$ROOT/macos/PasteTransaction.swift" "$ROOT/macos/TextInsertion.swift" "$ROOT/macos/InsertionPolicy.swift" "$ROOT/tests/workflow_native_adapter_test.swift" "${WORKFLOW_OBJECTS[@]}" -lc++ -framework AppKit -framework ApplicationServices -framework AVFoundation -framework Carbon -o "$OUT/workflow-native-adapter-test"
"$OUT/workflow-native-adapter-test"
APP="$OUT/LemonVox.app"
mkdir -p "$APP/Contents/MacOS"
xcrun swiftc -target "$SWIFT_TARGET" -module-cache-path "$OUT/ModuleCache" -swift-version 5 -warnings-as-errors -import-objc-header "$ROOT/core/workflow_bridge.h" "$ROOT/macos/UpdateSafety.swift" "$ROOT/macos/UpdateCoordinator.swift" "$ROOT/macos/LemonIcon.swift" "$ROOT/macos/ModifierTrigger.swift" "$ROOT/macos/TriggerControl.swift" "$ROOT/macos/WorkflowLaunchTracker.swift" "$ROOT/macos/WorkflowBridge.swift" "$ROOT/macos/WorkflowNativeAdapter.swift" "$ROOT/macos/main.swift" "$ROOT/macos/TextInsertion.swift" "$ROOT/macos/PasteTransaction.swift" "$ROOT/macos/InsertionPolicy.swift" "$ROOT/macos/TriggerChoice.swift" "$ROOT/macos/HotKey.swift" "$ROOT/macos/IndicatorPanel.swift" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/TransientAudio.swift" "$ROOT/macos/Recorder.swift" "$ROOT/macos/ConfigDialog.swift" "$ROOT/macos/ModelPicker.swift" "$ROOT/macos/ModelDiscovery.swift" "${WORKFLOW_OBJECTS[@]}" -lc++ -framework AppKit -framework Carbon -framework ApplicationServices -framework AVFoundation -F "$SPARKLE" -framework Sparkle -Xlinker -rpath -Xlinker @executable_path/../Frameworks -o "$APP/Contents/MacOS/LemonVoxPrototype"
cp "$ROOT/macos/Info.plist" "$APP/Contents/Info.plist"
printf 'Built %s\n' "$APP"

FIXTURE="$APP/Contents/Resources/LemonVox Keyboard Test.app"
mkdir -p "$FIXTURE/Contents/MacOS"
xcrun swiftc -target "$SWIFT_TARGET" -module-cache-path "$OUT/ModuleCache" -warnings-as-errors "$ROOT/fixtures/main.swift" "$ROOT/fixtures/FixtureEditMenu.swift" -framework AppKit -o "$FIXTURE/Contents/MacOS/LemonVoxKeyboardTest"
cat > "$FIXTURE/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>com.itomek.lemonvox.keyboardtest</string>
<key>CFBundleExecutable</key><string>LemonVoxKeyboardTest</string>
<key>CFBundleName</key><string>LemonVox Keyboard Test</string>
<key>CFBundlePackageType</key><string>APPL</string>
</dict></plist>
PLIST

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/ModelDiscovery.swift" "$ROOT/tests/settings_test.swift" "${CORE_OBJECTS[@]}" -lc++ -o "$OUT/settings-test"
"$OUT/settings-test"

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/macos/ModelDiscovery.swift" "$ROOT/macos/TriggerChoice.swift" "$ROOT/macos/TriggerControl.swift" "$ROOT/macos/ConfigDialog.swift" "$ROOT/macos/ModelPicker.swift" "$ROOT/tests/settings_ui_test.swift" "${CORE_OBJECTS[@]}" -lc++ -framework AppKit -o "$OUT/settings-ui-test"
"$OUT/settings-ui-test" --metadata-only
"$OUT/settings-ui-test" --diagnostics-only

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors "$ROOT/macos/ModelPicker.swift" "$ROOT/tests/model_picker_test.swift" -framework AppKit -o "$OUT/model-picker-test"

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors "$ROOT/macos/PasteTransaction.swift" "$ROOT/tests/paste_transaction_test.swift" -o "$OUT/paste-test"
"$OUT/paste-test"

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/protocol.h" "$ROOT/macos/TextInsertion.swift" "$ROOT/macos/PasteTransaction.swift" "$ROOT/macos/LemonadeClient.swift" "$ROOT/macos/Diagnostics.swift" "$ROOT/tests/system_clipboard_test.swift" "${CORE_OBJECTS[@]}" -lc++ -framework AppKit -framework ApplicationServices -framework Carbon -o "$OUT/system-clipboard-test"

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/session.h" "$ROOT/macos/TriggerChoice.swift" "$ROOT/tests/trigger_test.swift" "$OUT/session.o" -lc++ -o "$OUT/trigger-test"
"$OUT/trigger-test"
cp "$ROOT/scripts/render_icons.swift" "$OUT/main.swift"
xcrun swiftc -target "$SWIFT_TARGET" -module-cache-path "$OUT/ModuleCache" "$ROOT/macos/LemonIcon.swift" "$OUT/main.swift" -framework AppKit -o "$OUT/render-icons"
"$OUT/render-icons" "$OUT/LemonVox.iconset"
mkdir -p "$APP/Contents/Resources"
iconutil -c icns "$OUT/LemonVox.iconset" -o "$APP/Contents/Resources/LemonVox.icns"
cp "$ROOT/LICENSE" "$APP/Contents/Resources/LICENSE"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$APP/Contents/Resources/THIRD_PARTY_NOTICES.md"
cp "$ROOT/third_party/yyjson/LICENSE" "$APP/Contents/Resources/yyjson-LICENSE"

xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -import-objc-header "$ROOT/core/session.h" "$ROOT/macos/TriggerChoice.swift" "$ROOT/macos/ModifierTrigger.swift" "$ROOT/tests/modifier_trigger_test.swift" "$OUT/session.o" -lc++ -framework AppKit -framework ApplicationServices -o "$OUT/modifier-trigger-test"
"$OUT/modifier-trigger-test"

mkdir -p "$APP/Contents/Frameworks"
ditto "$SPARKLE/Sparkle.framework" "$APP/Contents/Frameworks/Sparkle.framework"
cp "$SPARKLE/LICENSE" "$APP/Contents/Resources/Sparkle-LICENSE"
xcrun swiftc -target "$SWIFT_TARGET" -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors "$ROOT/macos/UpdateSafety.swift" "$ROOT/tests/update_safety_test.swift" -o "$OUT/update-safety-test"
"$OUT/update-safety-test"

xcrun swiftc -target "$SWIFT_TARGET" -swift-version 5 -parse-as-library -module-cache-path "$OUT/ModuleCache" -warnings-as-errors -F "$SPARKLE" -framework Sparkle -Xlinker -rpath -Xlinker "$SPARKLE" "$ROOT/macos/UpdateSafety.swift" "$ROOT/macos/UpdateCoordinator.swift" "$ROOT/tests/update_coordinator_test.swift" -o "$OUT/update-coordinator-test"
"$OUT/update-coordinator-test"
