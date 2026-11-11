// SPDX-License-Identifier: GPL-3.0-or-later
// The assembled Storage test (Phase 4b design §3, §12, §15.1): the drive
// picker from the snapshot's `storage` field, the blocked (network) drive
// disabled with its reason, the free-space guard, the file-size option, and
// (task 11) a stub-scale run against a temp dir. Mirrors tst_ram_test.cpp.
#include "tests/storage/storage_test.h"
#include "tests/storage/req.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sb;
using namespace sb::tests;

namespace {

// A snapshot with two volumes: the root (SSD) and a network mount.
SystemSnapshot storageSnapshot()
{
    SystemSnapshot s;
    s.data.insert(QStringLiteral("storage"),
                  QJsonArray{
                      QJsonObject{{QStringLiteral("mount"), QStringLiteral("/")},
                                  {QStringLiteral("file_system"), QStringLiteral("btrfs")},
                                  {QStringLiteral("free_bytes"), 500.0e9},
                                  {QStringLiteral("total_bytes"), 1.0e12},
                                  {QStringLiteral("model"), QStringLiteral("Samsung 990 Pro")},
                                  {QStringLiteral("bus"), QStringLiteral("NVMe")},
                                  {QStringLiteral("kind"), QStringLiteral("SSD")}},
                      QJsonObject{{QStringLiteral("mount"), QStringLiteral("/mnt/nas")},
                                  {QStringLiteral("file_system"), QStringLiteral("nfs4")},
                                  {QStringLiteral("free_bytes"), 1.0e12},
                                  {QStringLiteral("total_bytes"), 4.0e12},
                                  {QStringLiteral("model"), QStringLiteral("NAS")},
                                  {QStringLiteral("bus"), QString()},
                                  {QStringLiteral("kind"), QString()}},
                  });
    return s;
}

struct Ctx : ProbeContext {
    SystemSnapshot s = storageSnapshot();
    const SystemSnapshot &system() const override { return s; }
    PrivilegedOps &privileged() const override { return const_cast<UnavailablePrivilegedOps &>(p); }

private:
    UnavailablePrivilegedOps p;
};

} // namespace

class TestStorageTest : public QObject
{
    Q_OBJECT

private slots:
    void identity()
    {
        StorageTest t;
        QCOMPARE(t.info().id, QStringLiteral("storage"));
        QCOMPARE(t.info().name, QStringLiteral("Storage"));
        QCOMPARE(t.info().colorToken, QStringLiteral("t-sto"));
        QCOMPARE(t.info().testVersion, 1);
        QStringList subs;
        for (const SubTestInfo &s : t.info().subTests) {
            subs << s.id;
            QVERIFY(!s.userSelectable);   // D15: all six, none selectable
        }
        QCOMPARE(subs, (QStringList{QStringLiteral("seq1m_q8t1"), QStringLiteral("seq1m_q1t1"),
                                    QStringLiteral("rnd4k_q32t1"), QStringLiteral("rnd4k_q1t1"),
                                    QStringLiteral("latency"), QStringLiteral("mixed")}));
    }

    void pickerAndOptions()
    {
        StorageTest t;
        Ctx ctx;
        const Availability a = t.probe(ctx);
        QVERIFY(a.state != Availability::State::Unavailable);

        const OptionSchema s = effectiveOptionSchema(t, ctx, a);
        const OptionDef *drive = s.find(QStringLiteral("drive"));
        QVERIFY(drive);
        QCOMPARE(drive->kind, OptionKind::DynamicSelect);
        QCOMPARE(drive->items.size(), 2);
        // The root volume is enabled and default.
        QCOMPARE(drive->items[0].value, QStringLiteral("/"));
        QVERIFY(drive->items[0].enabled);
        QCOMPARE(drive->defaultValue.toString(), QStringLiteral("/"));
        // The network mount is listed but disabled, with its reason.
        QCOMPARE(drive->items[1].value, QStringLiteral("/mnt/nas"));
        QVERIFY(!drive->items[1].enabled);
        QCOMPARE(drive->items[1].disabledReason, QStringLiteral("nfs4 · network — blocked"));

        const OptionDef *size = s.find(QStringLiteral("file_size"));
        QVERIFY(size);
        QCOMPARE(size->kind, OptionKind::SingleSelect);
        QCOMPARE(size->presentation, SelectPresentation::Segmented);
        QStringList values;
        for (const OptionItem &i : size->items)
            values << i.value;
        QCOMPARE(values, (QStringList{QStringLiteral("1g"), QStringLiteral("4g"), QStringLiteral("16g"), QStringLiteral("64g")}));
        QCOMPARE(size->defaultValue.toString(), QStringLiteral("4g"));
        // The stub's target_folder option is retired (D5), and no subtests checklist.
        QVERIFY(!s.find(QStringLiteral("target_folder")));
        QVERIFY(!s.find(QStringLiteral("subtests")));
        // The engine Info row names the compiled backend.
        QVERIFY(s.find(QStringLiteral("engine")));
    }

    void freeSpaceRefusalString()
    {
        StorageTest t;
        // validate() reads the free space live (QStorageInfo); the exact string
        // it emits is the pure req.h helper, checked here deterministically.
        const QString refusal = sb::storage::freeSpaceRefusal(4'400'000'000ll, 4'000'000'000ll);
        QCOMPARE(refusal, QStringLiteral("Not enough free space: the test needs about 6.1 GiB, the drive has 4 GiB"));
        // validate() on an empty drive is clean (no drive selected -> no guard).
        QVERIFY(t.validate({}).ok());
    }
};

QTEST_GUILESS_MAIN(TestStorageTest)
#include "tst_storage_test.moc"
