// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "scanner.h"
#include <QPointF>
#include <vector>

struct ScanActivityUpdate {
    QString path;
    qint64 totalBytesSeen = 0;
};

struct IncrementalRefreshResult {
    ScanResult refreshed;
    bool rootReplaced = false;
    std::vector<FileNode*> preparedFreeSpaceNodes;
    int subtreeDepth = 0;
    float subtreeBranchHue = 0.0f;
};
