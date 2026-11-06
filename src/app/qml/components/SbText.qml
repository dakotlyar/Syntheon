// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import Syntheon

// The one text item: theme colours and fonts, in the mockup's styles.
//   kind: "body" | "muted" | "small" | "label" | "data" | "display" | "title"
Text {
    id: root

    property string kind: "body"

    color: kind === "muted" || kind === "small" || kind === "label" ? Theme.muted : Theme.ink
    wrapMode: Text.NoWrap
    font.family: kind === "data" ? Theme.dataFamily
               : kind === "display" || kind === "title" ? Theme.displayFamily
               : Theme.bodyFamily
    font.pixelSize: kind === "title" ? 19
                  : kind === "display" ? 40
                  : kind === "small" || kind === "label" ? (kind === "label" ? 11 : 12)
                  : 14
    font.weight: kind === "title" || kind === "display" ? Font.Bold
               : kind === "label" ? Font.DemiBold
               : Font.Normal
    font.capitalization: kind === "label" ? Font.AllUppercase : Font.MixedCase
    font.letterSpacing: kind === "label" ? 0.9 : 0
    font.variableAxes: kind === "title" || kind === "display" ? Theme.displayAxes : ({})
    Accessible.role: Accessible.StaticText
    Accessible.name: text
}
