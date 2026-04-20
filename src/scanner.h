// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"
#include "treemapsnapshot.h"
#include "treemapsettings.h"
#include <algorithm>
#include <QString>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

struct ScanWarning {
    QString path;
    QString message;
};

struct FsInfo {
    QString canonicalMountRoot;   // e.g. "/" or "/home"
    QString displayMountRoot;     // as reported by QStorageInfo::rootPath()
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
    bool isLocal = true;          // false for NFS, CIFS, and other network filesystems
};

struct HardLinkTracker;
struct ScanThrottler;

struct ScanResult {
    std::shared_ptr<NodeArena> arena;
    std::shared_ptr<TreemapSnapshot> snapshot;
    FileNode* root = nullptr;   // owned by arena
    QString rootPath;
    qint64 freeBytes = 0;
    qint64 totalBytes = 0;
    qint64 fileCount = 0;
    QString currentScanPath;
    QVector<FsInfo> filesystems;  // per-filesystem data (empty on progress snapshots)
    std::shared_ptr<HardLinkTracker> hardLinkTracker;
};

inline void rebuildScanResultSnapshot(ScanResult& result)
{
    if (!result.root || !result.arena) {
        result.snapshot.reset();
        return;
    }

    result.snapshot = makeTreemapSnapshot(result.root, result.rootPath, result.arena, nextSnapshotGeneration());
}

class Scanner {
public:
    struct DirTask {
        FileNode* placeholder = nullptr;
        QString childPath;
        float branchHue = 0.0f;
        unsigned long long rootDev = 0;
        int depth = 0;
        bool inMarkedBranch = false;
    };

    struct PartitionTask {
        FileNode* parent = nullptr;
        QString path;
        float branchHue = 0.0f;
        unsigned long long rootDev = 0;
        int depth = 0;
        bool inMarkedBranch = false;
    };

    using ProgressCallback = std::function<void(ScanResult)>;
    using ProgressReadyCallback = std::function<bool()>;
    using ActivityCallback = std::function<void(const QString&, qint64)>;
    using ErrorCallback = std::function<void(const ScanWarning&)>;

    static ScanResult scan(const QString& path, const TreemapSettings& settings = TreemapSettings::defaults(),
                           ProgressCallback progressCallback = {},
                           ProgressReadyCallback progressReadyCallback = {},
                           ActivityCallback activityCallback = {},
                           ErrorCallback errorCallback = {},
                           std::shared_ptr<const std::atomic_bool> cancelFlag = nullptr);

private:
    static unsigned long long initialRootDevice(const QString& path);

    static void partitionNode(PartitionTask partition,
                              const TreemapSettings& settings,
                              const std::vector<QString>& allExcludedPaths,
                              const ErrorCallback& errorCallback,
                              const std::shared_ptr<const std::atomic_bool>& cancelFlag,
                              std::vector<PartitionTask>& partitionQueue,
                              std::vector<DirTask>& dirTasks,
                              NodeArena& arena,
                              const ActivityCallback& activityCallback,
                              const std::shared_ptr<std::atomic<qint64>>& liveBytesSeen,
                              ScanThrottler& throttler,
                              int targetTaskCount,
                              int effectiveParallelPartitionDepth,
                              const std::shared_ptr<HardLinkTracker>& hardLinkTracker);

    static qint64 scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                           const TreemapSettings& settings,
                           const std::vector<QString>& allExcludedPaths,
                           const ProgressReadyCallback& progressReadyCallback,
                           const ProgressCallback& progressCallback, NodeArena& arena,
                           const ActivityCallback& activityCallback,
                           const ErrorCallback& errorCallback,
                           float branchHue, unsigned long long rootDev, std::shared_ptr<const std::atomic_bool> cancelFlag = nullptr, int depth = 0,
                           bool inMarkedBranch = false,
                           ScanThrottler* throttler = nullptr,
                           bool alreadyThrottled = false);
};
