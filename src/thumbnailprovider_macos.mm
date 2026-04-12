// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "thumbnailprovider.h"

#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#include <dispatch/dispatch.h>
#include <CoreGraphics/CoreGraphics.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <QString>

QImage readThumbnailCache(const QString& path, int resolution)
{
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1String("/thumbnails");
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(path.toUtf8());
    hasher.addData(QByteArray::number(resolution));
    const QString cachedPath = cacheDir + QLatin1Char('/') + hasher.result().toHex() + QLatin1String(".jpg");

    QFileInfo cacheInfo(cachedPath);
    if (cacheInfo.exists() && cacheInfo.lastModified() >= QFileInfo(path).lastModified()) {
        return QImage(cachedPath);
    }
    return {};
}

void writeThumbnailCache(const QImage& img, const QString& path, int resolution)
{
    if (img.isNull()) return;
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QLatin1String("/thumbnails");
    QDir().mkpath(cacheDir);
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(path.toUtf8());
    hasher.addData(QByteArray::number(resolution));
    const QString cachedPath = cacheDir + QLatin1Char('/') + hasher.result().toHex() + QLatin1String(".jpg");
    img.save(cachedPath, "JPG");
}

QImage getSystemThumbnail(const QString& path, int size)
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:path.toNSString()];

        QLThumbnailGenerationRequest* request =
            [[QLThumbnailGenerationRequest alloc]
                    initWithFileAtURL:url
                                 size:CGSizeMake(size, size)
                                scale:1.0
                  representationTypes:QLThumbnailRepresentationTypeThumbnail];

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        __block CGImageRef cgResult = nullptr;

        [QLThumbnailGenerator.sharedGenerator
            generateBestRepresentationForRequest:request
            completionHandler:^(QLThumbnailRepresentation* rep, NSError*) {
                if (rep.CGImage) cgResult = CGImageRetain(rep.CGImage);
                dispatch_semaphore_signal(sem);
            }];

        // Wait up to 15 s; Quick Look is usually fast but some formats need decoding time.
        dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));

        if (!cgResult) return {};

        const int w = static_cast<int>(CGImageGetWidth(cgResult));
        const int h = static_cast<int>(CGImageGetHeight(cgResult));
        if (w <= 0 || h <= 0) { CGImageRelease(cgResult); return {}; }

        QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);

        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef ctx = CGBitmapContextCreate(
            img.bits(), w, h, 8, img.bytesPerLine(), cs,
            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host);
        CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), cgResult);
        CGContextRelease(ctx);
        CGColorSpaceRelease(cs);
        CGImageRelease(cgResult);

        return img.convertToFormat(QImage::Format_ARGB32);
    }
}
