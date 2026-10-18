// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

// How the Detail page groups its blocks into rows (Zeb M1): consecutive
// tables pair up unless one is fullWidth, so the CPU page's Sub-tests and
// Encryption tables are always alone, with or without the throttling note.
TestCase {
    id: test
    name: "DetailPage"
    width: 1000
    height: 600
    visible: true
    when: windowShown

    Component { id: pageComponent; DetailPage { width: 900 } }

    function table(title, fullWidth) { return { type: "table", title: title, columns: ["A"], rows: [], fullWidth: fullWidth } }
    function note() { return { type: "note", title: "", text: "n", tone: "warn" } }
    function shape(rows) {
        return rows.map(r => r.kind + ":" + r.blocks.map(b => b.title || b.type).join("+"))
    }

    function test_completedUnthrottled() {
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [{ type: "kpis", title: "", items: [] }, table("Sub-tests", true), table("Encryption", true), note(),
                        table("Compression", false), table("Single-thread", false)]
        compare(shape(page.group(blocks)), ["single:kpis", "single:Sub-tests", "single:Encryption", "single:note",
                                            "pair:Compression+Single-thread"])
    }

    function test_stoppedOrWorkerMissing() {
        // No Encryption table and no note: Sub-tests is next to Compression.
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Compression", false), table("Single-thread", false)]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "pair:Compression+Single-thread"])
    }

    function test_partialWithoutSingleThread() {
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Encryption", true), table("Compression", false), note()]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "single:Encryption", "single:Compression", "single:note"])
    }

    function test_plainTablesStillPair() {
        const page = createTemporaryObject(pageComponent, test)
        compare(shape(page.group([table("A"), table("B")])), ["pair:A+B"])
    }
}
