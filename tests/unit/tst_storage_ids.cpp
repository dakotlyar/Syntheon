// SPDX-License-Identifier: GPL-3.0-or-later
// The Storage test's pure ids/keys and requirement logic (Phase 4b design
// §6, §10, §16): every stable metric key, the pattern/field round-trips, and
// the network/free-space/wear classifiers.
#include "tests/storage/ids.h"
#include "tests/storage/req.h"

#include <QTest>

using namespace sb::storage;

class TestStorageIds : public QObject
{
    Q_OBJECT

private slots:
    void subTestIds()
    {
        QCOMPARE(patternId(Pattern::Seq1mQ8T1), QStringLiteral("seq1m_q8t1"));
        QCOMPARE(patternId(Pattern::Seq1mQ1T1), QStringLiteral("seq1m_q1t1"));
        QCOMPARE(patternId(Pattern::Rnd4kQ32T1), QStringLiteral("rnd4k_q32t1"));
        QCOMPARE(patternId(Pattern::Rnd4kQ1T1), QStringLiteral("rnd4k_q1t1"));
        QCOMPARE(patternId(Pattern::Latency), QStringLiteral("latency"));
        QCOMPARE(patternId(Pattern::Mixed), QStringLiteral("mixed"));
    }

    void patternKeys()
    {
        QCOMPARE(key(Pattern::Seq1mQ8T1, Field::Read), QStringLiteral("seq1m_q8t1.read"));
        QCOMPARE(key(Pattern::Seq1mQ8T1, Field::Write), QStringLiteral("seq1m_q8t1.write"));
        QCOMPARE(key(Pattern::Seq1mQ1T1, Field::Read), QStringLiteral("seq1m_q1t1.read"));
        QCOMPARE(key(Pattern::Seq1mQ1T1, Field::Write), QStringLiteral("seq1m_q1t1.write"));
        QCOMPARE(key(Pattern::Rnd4kQ32T1, Field::Read), QStringLiteral("rnd4k_q32t1.read"));
        QCOMPARE(key(Pattern::Rnd4kQ32T1, Field::ReadIops), QStringLiteral("rnd4k_q32t1.read_iops"));
        QCOMPARE(key(Pattern::Rnd4kQ32T1, Field::Write), QStringLiteral("rnd4k_q32t1.write"));
        QCOMPARE(key(Pattern::Rnd4kQ32T1, Field::WriteIops), QStringLiteral("rnd4k_q32t1.write_iops"));
        QCOMPARE(key(Pattern::Rnd4kQ1T1, Field::Read), QStringLiteral("rnd4k_q1t1.read"));
        QCOMPARE(key(Pattern::Rnd4kQ1T1, Field::ReadIops), QStringLiteral("rnd4k_q1t1.read_iops"));
        QCOMPARE(key(Pattern::Rnd4kQ1T1, Field::Write), QStringLiteral("rnd4k_q1t1.write"));
        QCOMPARE(key(Pattern::Rnd4kQ1T1, Field::WriteIops), QStringLiteral("rnd4k_q1t1.write_iops"));
        QCOMPARE(key(Pattern::Latency, Field::P50), QStringLiteral("latency.p50"));
        QCOMPARE(key(Pattern::Latency, Field::P99), QStringLiteral("latency.p99"));
        QCOMPARE(key(Pattern::Latency, Field::P999), QStringLiteral("latency.p999"));
        QCOMPARE(key(Pattern::Mixed, Field::Throughput), QStringLiteral("mixed.throughput"));
        QCOMPARE(key(Pattern::Mixed, Field::Iops), QStringLiteral("mixed.iops"));
    }

    void neutralKeys()
    {
        QCOMPARE(kModelIo, QStringLiteral("model.io"));
        QCOMPARE(kTargetDrive, QStringLiteral("target.drive"));
        QCOMPARE(kTargetFilesystem, QStringLiteral("target.filesystem"));
        QCOMPARE(kTargetFreeBytes, QStringLiteral("target.free_bytes"));
        QCOMPARE(kTargetKind, QStringLiteral("target.kind"));
        QCOMPARE(kFileSize, QStringLiteral("file_size"));
        QCOMPARE(kDirectAlignment, QStringLiteral("direct.alignment"));
        QCOMPARE(kPrepSeconds, QStringLiteral("prep.seconds"));
        QCOMPARE(kWearWriteGb, QStringLiteral("wear.write_gb"));
        QCOMPARE(kStabilitySpread, QStringLiteral("stability.spread"));
    }

    void unitsAndDirections()
    {
        QCOMPARE(unit(Pattern::Seq1mQ8T1, Field::Read), QStringLiteral("MB/s"));
        QCOMPARE(unit(Pattern::Rnd4kQ32T1, Field::ReadIops), QStringLiteral("IOPS"));
        QCOMPARE(unit(Pattern::Mixed, Field::Throughput), QStringLiteral("MB/s"));
        QCOMPARE(unit(Pattern::Latency, Field::P99), QStringLiteral("µs"));
        QVERIFY(lowerIsBetter(Pattern::Latency, Field::P50));
        QVERIFY(!lowerIsBetter(Pattern::Seq1mQ8T1, Field::Read));
        QVERIFY(fieldIsIops(Field::ReadIops));
        QVERIFY(!fieldIsIops(Field::Read));
    }

    void roundTrips()
    {
        for (Pattern p : kPatterns)
            QCOMPARE(patternFromId(QStringView(patternId(p))), std::optional<Pattern>(p));
        QVERIFY(!patternFromId(u"nope"));
        QCOMPARE(fieldFromKey(u"read"), std::optional<Field>(Field::Read));
        QCOMPARE(fieldFromKey(u"p999"), std::optional<Field>(Field::P999));
        QVERIFY(!fieldFromKey(u""));
    }

    void networkClassifier()
    {
        QVERIFY(isNetworkMount(QStringLiteral("/mnt/nas"), QStringLiteral("nfs4")));
        QVERIFY(isNetworkMount(QStringLiteral("/mnt/share"), QStringLiteral("cifs")));
        QVERIFY(isNetworkMount(QStringLiteral("/mnt/fuse"), QStringLiteral("fuse.sshfs")));
        QVERIFY(isNetworkMount(QStringLiteral("/mnt/s3"), QStringLiteral("S3FS")));  // case-insensitive
        QVERIFY(!isNetworkMount(QStringLiteral("/"), QStringLiteral("ext4")));
        QVERIFY(!isNetworkMount(QStringLiteral("/"), QStringLiteral("btrfs")));
        QVERIFY(isNetworkDriveType(4u));   // DRIVE_REMOTE
        QVERIFY(isNetworkDriveType(0u));   // DRIVE_UNKNOWN
        QVERIFY(!isNetworkDriveType(3u));  // DRIVE_FIXED
        QVERIFY(!isNetworkDriveType(2u));  // DRIVE_REMOVABLE (warned, not blocked, D2)
    }

    void freeSpaceGuard()
    {
        // free >= fileSize x 1.1 + 2 GiB.
        constexpr qint64 margin = 2ll * 1024 * 1024 * 1024;
        QVERIFY(hasEnoughFreeSpace(50ll * 1000000000, 4ll * 1000000000));
        // 4.4 GiB < 4 x 1.1 + 2 = 6.4 GiB.
        QVERIFY(!hasEnoughFreeSpace(4.4 * 1000000000, 4ll * 1000000000));
        // Exactly at the edge: fileSize x 1.1 + margin.
        const qint64 size = 4ll * 1000000000;
        QVERIFY(hasEnoughFreeSpace(size + size / 10 + margin, size));
        QVERIFY(!hasEnoughFreeSpace(size + size / 10 + margin - 1, size));
        QVERIFY(!hasEnoughFreeSpace(-1, size));
        QVERIFY(!hasEnoughFreeSpace(size, -1));
    }

    void worstCaseWrites()
    {
        QCOMPARE(worstCaseWriteBytes(4ll * 1000000000, 3), 4ll * 1000000000 * 4);
        QCOMPARE(worstCaseWriteBytes(4ll * 1000000000, 0), 4ll * 1000000000);
        QCOMPARE(worstCaseWriteBytes(0, 5), 0ll);
        QCOMPARE(worstCaseWriteBytes(4ll * 1000000000, -1), 0ll);
    }

    void scratchName()
    {
        const QUuid id = QUuid::createUuid();
        const QString name = targetScratchName(QStringLiteral("/data"), id);
        QVERIFY(name.startsWith(QStringLiteral("/data/.syntheon-storage-")));
        QVERIFY(name.endsWith(id.toString(QUuid::WithoutBraces)));
        QVERIFY(!name.contains(QStringLiteral("(.syntheon-storage-")));  // braced uuid, not bare
    }

    void rejectReasonTexts()
    {
        QCOMPARE(rejectReasonText(RejectReason::None), QString());
        QCOMPARE(rejectReasonText(RejectReason::Network), QStringLiteral("network — blocked"));
        QCOMPARE(rejectReasonText(RejectReason::ReadOnly), QStringLiteral("read-only"));
        QCOMPARE(rejectReasonText(RejectReason::TooSmall), QStringLiteral("too small"));
        QCOMPARE(rejectReasonText(RejectReason::NoFreeSpace), QStringLiteral("not enough free space"));
        QCOMPARE(rejectReasonText(RejectReason::NotFixed), QStringLiteral("not a fixed drive"));
    }

    void networkBlockStrings()
    {
        QCOMPARE(networkBlockReason(QStringLiteral("CIFS")), QStringLiteral("cifs · network — blocked"));
        QCOMPARE(networkBlockReason(QStringLiteral("nfs4")), QStringLiteral("nfs4 · network — blocked"));
        QCOMPARE(networkBlockReason(QStringLiteral("NFS")), QStringLiteral("nfs · network — blocked"));
    }

    void freeSpaceRefusalStrings()
    {
        // 4 GiB needs 4 x 1.1 + 2 GiB margin ≈ 6.5 GiB (6.1 GiB, one decimal); a
        // 4.4 GiB drive is short.
        const QString r = freeSpaceRefusal(4'400'000'000ll, 4'000'000'000ll);
        QVERIFY(r.startsWith(QStringLiteral("Not enough free space:")));
        QVERIFY(r.contains(QStringLiteral("6.1 GiB")));  // the "needs about" figure
        QVERIFY(r.contains(QStringLiteral("4.1 GiB")));  // the "has" figure
        // A drive with exactly enough free reports the "has" as its own GiB
        // (and its "needs about" the same: free == the threshold).
        const qint64 size = 4'000'000'000ll;
        const qint64 enough = size + size / 10 + kFreeMarginBytes;
        const QString ok = freeSpaceRefusal(enough, size);
        QVERIFY(ok.contains(QStringLiteral("6.1 GiB")));  // 6.547 GiB, one decimal
    }

    void wearNoteStrings()
    {
        QCOMPARE(wearNote(8'000'000'000ll, QStringLiteral("SSD")), QStringLiteral("writes about 8 GB to this SSD"));
        QCOMPARE(wearNote(8'000'000'000ll, QStringLiteral("HDD")), QStringLiteral("writes about 8 GB to this drive"));
        QCOMPARE(wearNote(8'000'000'000ll, QStringLiteral("NVMe")), QStringLiteral("writes about 8 GB to this drive"));
        QCOMPARE(wearNote(0, QStringLiteral("SSD")), QStringLiteral("writes about 0 GB to this SSD"));
        QCOMPARE(wearNote(8'499'999'999ll, QStringLiteral("SSD")), QStringLiteral("writes about 8 GB to this SSD"));
        QCOMPARE(wearNote(8'500'000'001ll, QStringLiteral("SSD")), QStringLiteral("writes about 9 GB to this SSD"));
    }
};

QTEST_GUILESS_MAIN(TestStorageIds)
#include "tst_storage_ids.moc"