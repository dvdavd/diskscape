// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "mainwindow.h"
#include "mainwindow_utils.h"
#include "version.h"
#include <QIcon>
#include <QApplication>
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QStyleFactory>
#include <QTranslator>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);

    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
                          QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);

    QTranslator appTranslator;
    if (appTranslator.load(QLocale(), QStringLiteral("diskvu"), QStringLiteral("_"),
                           QStringLiteral(":/i18n")))
        app.installTranslator(&appTranslator);

#ifdef Q_OS_WIN
    if (QStyleFactory::keys().contains(QStringLiteral("fusion"), Qt::CaseInsensitive)) {
        app.setStyle(QStyleFactory::create(QStringLiteral("fusion")));
    }
#endif
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Diskvu"));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.dvdavd.Diskvu"));
    app.setApplicationVersion(QString::fromLatin1(DISKVU_VERSION));
    app.setOrganizationName(QStringLiteral("diskvu"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("io.github.dvdavd.Diskvu"),
                                       QIcon(QStringLiteral(":/assets/diskvu_appicon.svg"))));
    app.setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont), "QMenu");
    syncApplicationPaletteToColorScheme(app, systemUsesDarkColorScheme());
    QCommandLineParser parser;
    parser.setApplicationDescription("Diskvu");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addPositionalArgument("path", "Directory to scan on startup.");
    parser.process(app);

    QString initialPath;
    const QStringList positionalArgs = parser.positionalArguments();
    if (!positionalArgs.isEmpty()) {
        initialPath = positionalArgs.constFirst().trimmed();
    }

    MainWindow window;
    window.show();
    if (!initialPath.isEmpty()) {
        window.openInitialPath(initialPath);
    }

    return app.exec();
}
