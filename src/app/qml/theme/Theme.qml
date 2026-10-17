// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

// Design tokens taken exactly from the mockup CSS (§7). Light and dark; "system"
// follows the OS and updates live. Where the platform reports Unknown, light
// is used (R6).
QtObject {
    id: theme

    // "system" | "light" | "dark"; bound to Settings.theme by Main.qml.
    property string mode: "system"

    readonly property bool dark: mode === "dark"
        || (mode === "system" && Application.styleHints.colorScheme === Qt.Dark)

    function pick(light, darkValue) { return dark ? darkValue : light }

    readonly property color page: pick("#e8ecef", "#0b0f13")
    readonly property color win: pick("#f7f9fa", "#121920")
    readonly property color panel: pick("#ffffff", "#18212a")
    readonly property color sunk: pick("#eef2f4", "#0f151b")
    readonly property color ink: pick("#15202a", "#e4ebf0")
    readonly property color muted: pick("#5a6875", "#91a0ad")
    readonly property color line: pick("#d5dde3", "#2a3640")
    readonly property color accent: pick("#0b7480", "#3fc1cc")
    readonly property color accentInk: pick("#ffffff", "#062a2e")
    readonly property color accentSoft: pick("#d8eef0", "#123a3f")
    readonly property color good: pick("#1f7a3d", "#5cc884")
    readonly property color warn: pick("#9a5b00", "#e3a64a")
    readonly property color bad: pick("#b3261e", "#f2867d")
    readonly property color na: pick("#7b8794", "#7f8d99")

    readonly property color tCpu: pick("#2f6fd6", "#6fa3ff")
    readonly property color tRam: pick("#8a4fd0", "#b98cf2")
    readonly property color tSto: pick("#c26a12", "#f0a356")
    readonly property color tGpu: pick("#148a5a", "#4fcf95")
    readonly property color tAi: pick("#c2386e", "#f07aa8")
    readonly property color tNpu: pick("#5d6bd8", "#97a2f5")
    readonly property color tBat: pick("#8a7a12", "#d9c752")

    readonly property int radius: 6

    // Test colour tokens ("t-cpu") come from the tests' metadata.
    function testColor(token) {
        switch (token) {
        case "t-cpu": return tCpu
        case "t-ram": return tRam
        case "t-sto": return tSto
        case "t-gpu": return tGpu
        case "t-ai": return tAi
        case "t-npu": return tNpu
        case "t-bat": return tBat
        default: return muted
        }
    }

    // Pill/tone names from the C++ side: neutral|good|warn|bad|na (and "acc").
    function toneColor(tone) {
        switch (tone) {
        case "good": return good
        case "warn": return warn
        case "bad": return bad
        case "na": return na
        case "acc": return accent
        default: return muted
        }
    }

    // Fonts: display = Archivo (87 % width, 700); body = IBM Plex Sans; data =
    // IBM Plex Mono with tabular figures. The files are bundled and loaded in
    // C++ (loadBundledFonts); the generic fallbacks keep tests readable.
    readonly property string displayFamily: "Archivo"
    readonly property string bodyFamily: "IBM Plex Sans"
    readonly property string dataFamily: "IBM Plex Mono"
    readonly property var displayAxes: ({ "wdth": 87 })
}
