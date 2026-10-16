// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtTest
import SynBench

// The mockup's tokens, light and dark, and the explicit modes.
TestCase {
    name: "Theme"

    function cleanup() { Theme.mode = "system" }

    function test_lightTokens() {
        Theme.mode = "light"
        verify(!Theme.dark)
        compare(Theme.page, "#e8ecef")
        compare(Theme.win, "#f7f9fa")
        compare(Theme.panel, "#ffffff")
        compare(Theme.ink, "#15202a")
        compare(Theme.accent, "#0b7480")
        compare(Theme.accentSoft, "#d8eef0")
        compare(Theme.tCpu, "#2f6fd6")
        compare(Theme.tBat, "#8a7a12")
        compare(Theme.good, "#1f7a3d")
    }

    function test_darkTokens() {
        Theme.mode = "dark"
        verify(Theme.dark)
        compare(Theme.page, "#0b0f13")
        compare(Theme.panel, "#18212a")
        compare(Theme.accent, "#3fc1cc")
        compare(Theme.accentInk, "#062a2e")
        compare(Theme.tRam, "#b98cf2")
        compare(Theme.bad, "#f2867d")
    }

    function test_switchingUpdatesLive() {
        Theme.mode = "light"
        const before = String(Theme.ink)
        Theme.mode = "dark"
        verify(String(Theme.ink) !== before)
        Theme.mode = "light"
        compare(String(Theme.ink), before)
    }

    function test_testColorsFollowTheTheme() {
        Theme.mode = "light"
        compare(Theme.testColor("t-gpu"), Theme.tGpu)
        compare(Theme.testColor("t-npu"), Theme.tNpu)
        compare(Theme.testColor("unknown"), Theme.muted)
        compare(Theme.toneColor("warn"), Theme.warn)
        compare(Theme.toneColor("acc"), Theme.accent)
    }

    function test_fontFamilies() {
        compare(Theme.displayFamily, "Archivo")
        compare(Theme.bodyFamily, "IBM Plex Sans")
        compare(Theme.dataFamily, "IBM Plex Mono")
    }
}
