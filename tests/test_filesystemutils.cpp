// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filesystemutils.h"

#include <QtTest/QtTest>

class TestFilesystemUtils : public QObject {
    Q_OBJECT

private slots:
    void knownNetworkFsTypes_areNonLocal()
    {
        QVERIFY(!isLocalFilesystem(QStringLiteral("nfs"), QByteArrayLiteral("/dev/disk1s1"), QStringLiteral("/")));
        QVERIFY(!isLocalFilesystem(QStringLiteral("smbfs"), QByteArrayLiteral("/dev/disk1s1"), QStringLiteral("/")));
        QVERIFY(!isLocalFilesystem(QStringLiteral("cifs"), QByteArrayLiteral("/dev/disk1s1"), QStringLiteral("/")));
        QVERIFY(!isLocalFilesystem(QStringLiteral("sshfs"), QByteArrayLiteral("/dev/disk1s1"), QStringLiteral("/")));
    }

    void unixStyleNetworkDevicePrefixes_areNonLocal()
    {
#ifdef Q_OS_WIN
        QSKIP("Unix-style device prefixes are not used on Windows.");
#else
        QVERIFY(!isLocalFilesystem(QString(), QByteArrayLiteral("//server/share"), QStringLiteral("/Volumes/share")));
        QVERIFY(!isLocalFilesystem(QString(), QByteArray(), QStringLiteral("//server/share")));
#endif
    }

    void localFilesystemExamples_areLocal()
    {
#ifdef Q_OS_WIN
        QVERIFY(isLocalFilesystem(QStringLiteral("NTFS"), QByteArrayLiteral("\\Device\\HarddiskVolume3"), QStringLiteral("C:/")));
        QVERIFY(isLocalFilesystem(QString(), QByteArrayLiteral("C:"), QStringLiteral("C:/")));
#else
        QVERIFY(isLocalFilesystem(QStringLiteral("apfs"), QByteArrayLiteral("/dev/disk3s1"), QStringLiteral("/")));
        QVERIFY(isLocalFilesystem(QStringLiteral("ext4"), QByteArrayLiteral("/dev/nvme0n1p2"), QStringLiteral("/")));
#endif
    }

    void windowsUncAndNetworkFsTypes_areNonLocal()
    {
#ifdef Q_OS_WIN
        QVERIFY(!isLocalFilesystem(QStringLiteral("NTFS"), QByteArrayLiteral("\\\\server\\share"), QStringLiteral("Z:/")));
        QVERIFY(!isLocalFilesystem(QStringLiteral("NFS"), QByteArrayLiteral("\\Device\\HarddiskVolume3"), QStringLiteral("N:/")));
#else
        QSKIP("Windows-specific UNC handling.");
#endif
    }
};

QTEST_MAIN(TestFilesystemUtils)
#include "test_filesystemutils.moc"
