// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "settingsdialog.h"
#include "treemapwidget.h"

#include <QApplication>
#include <QtTest/QtTest>

class TestSettingsDialog : public QObject {
    Q_OBJECT

private slots:
    void previewSeedsSampleThumbnailUsingPreviewNodePath()
    {
        const TreemapSettings settings;
        SettingsDialog dialog(settings);
        TreemapWidget* const previewWidget = dialog.findChild<TreemapWidget*>();

        QVERIFY(previewWidget != nullptr);
        QVERIFY(previewWidget->hasCachedThumbnailForPath(QStringLiteral("Root/assets/sample.jpg")));
    }
};

QTEST_MAIN(TestSettingsDialog)
#include "test_settingsdialog.moc"
