// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filesystemwatchcontroller.h"

#include "scanner.h"
#include "treemapsettings.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace {
bool writeSizedFile(const QString& path, qint64 size)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }

    QByteArray chunk(64 * 1024, '@');
    qint64 remaining = size;
    while (remaining > 0) {
        const qint64 toWrite = std::min<qint64>(remaining, chunk.size());
        if (file.write(chunk.constData(), toWrite) != toWrite) {
            return false;
        }
        remaining -= toWrite;
    }

    return file.flush();
}
} // namespace

class TestFilesystemWatchController : public QObject {
    Q_OBJECT

private slots:
    void emitsRefreshAgainAfterTreeContextIsRebuilt()
    {
        QTemporaryDir watchedDir;
        QTemporaryDir stagingDir;
        QVERIFY(watchedDir.isValid());
        QVERIFY(stagingDir.isValid());

        const QString watchedPath = watchedDir.path();
        const QString fileName = QStringLiteral("tracked.bin");
        const QString watchedFile = watchedPath + QLatin1Char('/') + fileName;
        const QString stagedFile = stagingDir.path() + QLatin1Char('/') + fileName;
        QVERIFY(writeSizedFile(watchedFile, 2 * 1024 * 1024));

        TreemapSettings settings = TreemapSettings::defaults();
        settings.scanPreviewMode = TreemapSettings::ScanPreviewNone;

        ScanResult initial = Scanner::scan(watchedPath, settings);
        QVERIFY(initial.root);

        FilesystemWatchController controller;
        QSignalSpy spy(&controller, &FilesystemWatchController::refreshRequested);
        controller.setTreeContext(initial.root, initial.root);

        QVERIFY(QFile::rename(watchedFile, stagedFile));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);
        QCOMPARE(spy.takeFirst().at(0).toString(), watchedPath);

        ScanResult refreshed = Scanner::scan(watchedPath, settings);
        QVERIFY(refreshed.root);
        controller.setTreeContext(refreshed.root, refreshed.root);

        QVERIFY(QFile::rename(stagedFile, watchedFile));
        QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);
        QCOMPARE(spy.takeFirst().at(0).toString(), watchedPath);
    }
};

QTEST_MAIN(TestFilesystemWatchController)
#include "test_filesystemwatchcontroller.moc"
