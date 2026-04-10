// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QByteArray>
#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>

QList<qreal> speculativeDevicePixelRatios();
QByteArray normalizeTablerSvg(QByteArray svgData);
QIcon makeRecoloredSvgIcon(const QString& svgPath, const QColor& color);
void clearRecoloredSvgCache();
