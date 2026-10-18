// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtTest
import SynBench

// How the Detail page groups its blocks into rows (Zeb M1): consecutive
// tables pair up unless one is fullWidth. Every CPU table is fullWidth (Dave,
// 2026-10-18), so each is alone on its row, and the all-threads note sits
// right under the Single-thread table.
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
                        table("Compression", true), table("Single-thread", true), note()]
        compare(shape(page.group(blocks)), ["single:kpis", "single:Sub-tests", "single:Encryption", "single:note",
                                            "single:Compression", "single:Single-thread", "single:note"])
    }

    function test_stoppedOrWorkerMissing() {
        // No Encryption table and no note: still one table per row.
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Compression", true), table("Single-thread", true), note()]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "single:Compression", "single:Single-thread", "single:note"])
    }

    function test_partialWithoutSingleThread() {
        const page = createTemporaryObject(pageComponent, test)
        const blocks = [table("Sub-tests", true), table("Encryption", true), table("Compression", true), note()]
        compare(shape(page.group(blocks)), ["single:Sub-tests", "single:Encryption", "single:Compression", "single:note"])
    }

    function test_plainTablesStillPair() {
        const page = createTemporaryObject(pageComponent, test)
        compare(shape(page.group([table("A"), table("B")])), ["pair:A+B"])
    }
}
