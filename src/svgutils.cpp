// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "svgutils.h"

#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QScreen>
#include <QSvgRenderer>
#include <algorithm>
#include <cmath>

static QHash<QString, QIcon> s_recoloredSvgCache;

QList<qreal> speculativeDevicePixelRatios()
{
    QList<qreal> ratios = {
        1.0, 1.25, 1.5, 1.75,
        2.0, 2.25, 2.5, 3.0, 4.0
    };

    if (qApp) {
        const auto screens = qApp->screens();
        for (const QScreen* screen : screens) {
            if (!screen) {
                continue;
            }
            ratios.append(screen->devicePixelRatio());
        }
    }

    std::sort(ratios.begin(), ratios.end());
    ratios.erase(std::unique(ratios.begin(), ratios.end(), [](qreal a, qreal b) {
        return qAbs(a - b) < 0.01;
    }), ratios.end());
    return ratios;
}

QByteArray normalizeTablerSvg(QByteArray svgData)
{
    svgData.replace("stroke-width=\"1\"", "stroke-width=\"1.5\"");
    return svgData;
}

static QPixmap renderSvgToPixmap(QSvgRenderer& renderer, int logicalSize, qreal dpr)
{
    QPixmap pixmap(qMax(1, qRound(logicalSize * dpr)), qMax(1, qRound(logicalSize * dpr)));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    renderer.render(&painter, QRectF(0, 0, logicalSize, logicalSize));
    return pixmap;
}

QIcon makeRecoloredSvgIcon(const QString& svgPath, const QColor& color)
{
    const QColor disabledColor = qApp
        ? qApp->palette().color(QPalette::Disabled, QPalette::ButtonText)
        : color;
    const QString key = svgPath + QLatin1Char(':')
        + color.name(QColor::HexArgb) + QLatin1Char(':')
        + disabledColor.name(QColor::HexArgb);
    auto it = s_recoloredSvgCache.find(key);
    if (it != s_recoloredSvgCache.end())
        return it.value();

    QFile file(svgPath);
    if (!file.open(QIODevice::ReadOnly))
        return QIcon();
    const QByteArray svgTemplate = normalizeTablerSvg(file.readAll());

    auto recolor = [&](const QColor& c) {
        QByteArray data = svgTemplate;
        data.replace(QByteArrayLiteral("currentColor"), c.name(QColor::HexRgb).toUtf8());
        return data;
    };

    const QByteArray normalSvg = recolor(color);
    const QByteArray disabledSvg = recolor(disabledColor);
    QSvgRenderer normalRenderer(normalSvg);
    QSvgRenderer disabledRenderer(disabledSvg);
    if (!normalRenderer.isValid() || !disabledRenderer.isValid())
        return QIcon();

    const QList<int> logicalSizes = {16, 18, 20, 22, 24, 32, 48};
    const QList<qreal> dprs = speculativeDevicePixelRatios();
    QIcon icon;
    for (const qreal dpr : dprs) {
        for (int logicalSize : logicalSizes) {
            icon.addPixmap(renderSvgToPixmap(normalRenderer, logicalSize, dpr), QIcon::Normal);
            icon.addPixmap(renderSvgToPixmap(disabledRenderer, logicalSize, dpr), QIcon::Disabled);
        }
    }

    s_recoloredSvgCache.insert(key, icon);
    return icon;
}

void clearRecoloredSvgCache()
{
    s_recoloredSvgCache.clear();
}
