// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "thumbnailprovider.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

// XDG thumbnail spec: filename = md5("file://<path>").hex + ".png"
static QString xdgCachePath(const QByteArray& uri, const QString& dir)
{
    const QString hash = QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex();
    return QDir::homePath() + "/.cache/thumbnails/" + dir + "/" + hash + ".png";
}

QImage checkXdgThumbnailCache(const QString& path)
{
    const QByteArray uri = ("file://" + path).toUtf8();
    const qint64 actualMtime = QFileInfo(path).lastModified().toSecsSinceEpoch();
    for (const char* dir : {"x-large", "large", "normal"}) {
        const QImage cached(xdgCachePath(uri, QLatin1String(dir)));
        if (cached.isNull()) continue;
        if (cached.text(QStringLiteral("Thumb::MTime")).toLongLong() == actualMtime)
            return cached;
    }
    return {};
}

void writeXdgThumbnailCache(const QImage& img, const QString& path, int resolution)
{
    const char* tier = resolution <= 128 ? "normal"
                     : resolution <= 256 ? "large"
                     : resolution <= 512 ? "x-large"
                     :                     "xx-large";
    const int tileSize = resolution <= 128 ? 128
                       : resolution <= 256 ? 256
                       : resolution <= 512 ? 512
                       :                    1024;
    const QString cacheDir = QDir::homePath() + QStringLiteral("/.cache/thumbnails/") + QLatin1String(tier) + QLatin1Char('/');
    if (!QDir().mkpath(cacheDir)) return;
    const QByteArray uri = ("file://" + path).toUtf8();
    QImage toSave = img.scaled(tileSize, tileSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    toSave.setText(QStringLiteral("Thumb::URI"),   QString::fromUtf8(uri));
    toSave.setText(QStringLiteral("Thumb::MTime"),
                   QString::number(QFileInfo(path).lastModified().toSecsSinceEpoch()));
    toSave.save(xdgCachePath(uri, QLatin1String(tier)), "PNG");
}

QImage readThumbnailCache(const QString& path, int resolution)
{
    const QImage cached = checkXdgThumbnailCache(path);
    if (!cached.isNull())
        return cached.scaled(resolution, resolution, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return {};
}

void writeThumbnailCache(const QImage& img, const QString& path, int resolution)
{
    if (!img.isNull())
        writeXdgThumbnailCache(img, path, resolution);
}

QImage getSystemThumbnail(const QString& path, int size)
{
    // Try ffmpegthumbnailer; fall back to ffmpeg.
    const QString ffmpegthumbnailer =
        QStandardPaths::findExecutable(QStringLiteral("ffmpegthumbnailer"));
    const QString ffmpeg = ffmpegthumbnailer.isEmpty()
        ? QStandardPaths::findExecutable(QStringLiteral("ffmpeg"))
        : QString();

    if (ffmpegthumbnailer.isEmpty() && ffmpeg.isEmpty())
        return {};

    const QByteArray uri = ("file://" + path).toUtf8();
    const QString hash = QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex();
    const QString tmpPath = QDir::tempPath() + "/diskscape_vthumb_" + hash + ".png";

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);

    if (!ffmpegthumbnailer.isEmpty()) {
        proc.start(ffmpegthumbnailer, {
            QStringLiteral("-i"), path,
            QStringLiteral("-o"), tmpPath,
            QStringLiteral("-s"), QString::number(qMax(size, 256)),
            QStringLiteral("-c"), QStringLiteral("png"),
            QStringLiteral("-t"), QStringLiteral("10%"),
            QStringLiteral("-f"),   // overwrite existing
        });
    } else {
        proc.start(ffmpeg, {
            QStringLiteral("-y"),
            QStringLiteral("-i"), path,
            QStringLiteral("-ss"), QStringLiteral("5"),
            QStringLiteral("-vframes"), QStringLiteral("1"),
            QStringLiteral("-vf"), QStringLiteral("scale=%1:-1").arg(qMax(size, 256)),
            tmpPath,
        });
    }

    const bool finished = proc.waitForFinished(15000);
    if (!finished || proc.exitCode() != 0) {
        QFile::remove(tmpPath);
        return {};
    }

    QImage result(tmpPath);
    QFile::remove(tmpPath);
    return result.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}
