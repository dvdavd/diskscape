// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QImage>
#include <QString>

// Returns a thumbnail image for the given file using system-provided providers
// (QuickLook on macOS, Shell Image Factory on Windows, ffmpegthumbnailer on Linux).
// Scaled to fit within size × size pixels. Returns a null QImage on failure.
//
// May block for up to ~15 s. Call only from a background thread.
QImage getSystemThumbnail(const QString& path, int size);

// Unified thumbnail cache helpers. Both are safe to call from a background
// thread (they only touch the filesystem or use platform-standard locations).
//
// Read returns a null image on miss or stale mtime (if supported by platform).
QImage readThumbnailCache(const QString& path, int resolution);

// Write saves the image to the appropriate system or application cache directory.
void writeThumbnailCache(const QImage& img, const QString& path, int resolution);

#if defined(Q_OS_UNIX) && !defined(Q_OS_DARWIN)
// XDG thumbnail cache internal helpers.
QImage checkXdgThumbnailCache(const QString& path);
void writeXdgThumbnailCache(const QImage& img, const QString& path, int resolution);
#endif
