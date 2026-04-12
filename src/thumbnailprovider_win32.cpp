// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "thumbnailprovider.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

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
    // COM must be initialised on this thread.
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    IShellItemImageFactory* factory = nullptr;
    const QString nativePath = QDir::toNativeSeparators(path);
    HRESULT hr = SHCreateItemFromParsingName(
        reinterpret_cast<LPCWSTR>(nativePath.utf16()), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return {};
    }

    // SIIGBF_BIGGERSIZEOK: return the closest size >= requested.
    // SIIGBF_THUMBNAILONLY: skip icon fallback — return null rather than an icon.
    // SIIGBF_SCALEUP: allow scaling up for small videos/images.
    HBITMAP hbmp = nullptr;
    hr = factory->GetImage({size, size},
                           SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY | SIIGBF_SCALEUP, &hbmp);
    factory->Release();
    if (FAILED(hr) || !hbmp) {
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return {};
    }

    BITMAP bm = {};
    if (!GetObject(hbmp, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        DeleteObject(hbmp);
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return {};
    }

    // GetDIBits with BI_RGB writes pixels as B, G, R, 0 in memory.
    // Qt ARGB32 on little-endian reads bytes as B(0) G(1) R(2) A(3) — same order,
    // so the RGB channels are already correct; we just need to set alpha to 0xFF.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = bm.bmWidth;
    bmi.bmiHeader.biHeight      = -bm.bmHeight;  // negative = top-down scan lines
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    QImage img(bm.bmWidth, bm.bmHeight, QImage::Format_ARGB32);
    HDC hdc = GetDC(nullptr);
    GetDIBits(hdc, hbmp, 0, static_cast<UINT>(bm.bmHeight), img.bits(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, hdc);
    DeleteObject(hbmp);

    // BI_RGB leaves the alpha byte as 0; set it to fully opaque.
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x)
            line[x] |= 0xFF000000u;
    }

    if (SUCCEEDED(hrCom)) CoUninitialize();
    return img;
}
