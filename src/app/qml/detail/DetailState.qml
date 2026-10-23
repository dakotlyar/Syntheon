// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick

// Which Detail-page tables are open (Phase 4a D25): per test id and table
// title, for as long as the app runs; never saved. Bindings that call
// isExpanded() also read `revision`, which changes on every write.
QtObject {
    property var expanded: ({})
    property int revision: 0
    function isExpanded(key) { return expanded[key] === true }
    function setExpanded(key, on) { if (on) expanded[key] = true; else delete expanded[key]; ++revision }
    function setAll(keys, on) {
        for (const k of keys) { if (on) expanded[k] = true; else delete expanded[k] }
        ++revision
    }
    function clear() { expanded = ({}); ++revision }  // tests
}
