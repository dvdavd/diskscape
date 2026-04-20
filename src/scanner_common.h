// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

// Shared constants and helpers used by scanner_posix.cpp and scanner_win32.cpp.
// Placed in an anonymous namespace so each translation unit gets its own copy
// (required for the thread_local statics inside emitProgress).

#include "scanner.h"
#include "filesystemutils.h"

#include <QDir>
#include <QElapsedTimer>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QSemaphore>
#include <QHash>
#include <QStorageInfo>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

struct HardLinkKey {
    unsigned long long dev;
    unsigned long long ino;

    bool operator==(const HardLinkKey& other) const
    {
        return dev == other.dev && ino == other.ino;
    }
};

namespace std {
template<> struct hash<HardLinkKey> {
    size_t operator()(const HardLinkKey& key) const
    {
        return hash<unsigned long long>{}(key.dev) ^ (hash<unsigned long long>{}(key.ino) << 1);
    }
};
}

struct HardLinkTracker {
    static constexpr int kShards = 64;

    struct Shard {
        std::mutex mutex;
        std::unordered_set<HardLinkKey> seen;
    };
    Shard shards[kShards];

    Shard& shardFor(unsigned long long ino)
    {
        return shards[ino % kShards];
    }
};

namespace {

bool shouldCountHardLink(const std::shared_ptr<HardLinkTracker>& tracker,
                         unsigned long long dev, unsigned long long ino)
{
    if (!tracker) {
        return true;
    }

    auto& shard = tracker->shardFor(ino);
    std::lock_guard<std::mutex> lock(shard.mutex);
    return shard.seen.insert({dev, ino}).second;
}

QString spaceSharingDeviceId(const QStorageInfo& vol)
{
    const QString device = vol.device();
#ifdef Q_OS_MACOS
    if (vol.fileSystemType() == "apfs" && device.startsWith(QLatin1String("/dev/disk"))) {
        // Normalize /dev/disk3s1 to /dev/disk3 to represent the container
        const int sIndex = device.indexOf(QLatin1Char('s'), 9); // After /dev/disk
        if (sIndex != -1) {
            return device.left(sIndex);
        }
    }
#endif
    return device;
}

} // namespace

struct ScanThrottler {
    QSemaphore networkSemaphore{2};
    QMutex mutex;
    QHash<unsigned long long, bool> isLocalCache;

    bool isLocal(unsigned long long dev, const QString& path)
    {
        QMutexLocker locker(&mutex);
        auto it = isLocalCache.find(dev);
        if (it != isLocalCache.end()) {
            return *it;
        }
        bool local = isLocalFilesystem(QStorageInfo(path));
        isLocalCache.insert(dev, local);
        return local;
    }
};

struct ThrottleGuard {
    QSemaphore* sem;
    bool active;
    ThrottleGuard(QSemaphore* s, bool a)
        : sem(s)
        , active(a)
    {
        if (active && sem) {
            sem->acquire(1);
        }
    }
    ~ThrottleGuard()
    {
        if (active && sem) {
            sem->release(1);
        }
    }
};

namespace {

static constexpr int kMaxDepth = 64;
static constexpr qint64 kProgressIntervalMs = 180;
static constexpr qint64 kActivityIntervalMs = 40;
static constexpr int kPreviewDepth = 5;
static constexpr size_t kPreviewChildLimits[] = {
    16,
    24,
    64,
    160,
    384,
};

QStringList defaultExcludedPathsForScanRoot(const QString& scanRootPath)
{
    if (QDir::cleanPath(scanRootPath) != QLatin1String("/")) {
        return {};
    }

#ifdef Q_OS_LINUX
    return {
        QStringLiteral("/proc"),
        QStringLiteral("/sys"),
        QStringLiteral("/dev"),
        QStringLiteral("/run"),
    };
#elif defined(Q_OS_MACOS)
    return {
        QStringLiteral("/dev"),
        // macOS exposes implementation-detail mounts under /System/Volumes
        // (Data, VM, Preboot, Update, etc.). Skip them during root scans and
        // prefer the user-facing aliases and mount points elsewhere.
        QStringLiteral("/System/Volumes"),
    };
#else
    return {};
#endif
}

QString childPathForParent(const QString& parentPath, const QString& childName)
{
    if (parentPath.endsWith(QLatin1Char('/'))) {
        return parentPath + childName;
    }
    return parentPath + QLatin1Char('/') + childName;
}

QString childPathPrefixForParent(const QString& parentPath)
{
    if (parentPath.endsWith(QLatin1Char('/'))) {
        return parentPath;
    }
    QString prefix = parentPath;
    prefix += QLatin1Char('/');
    return prefix;
}

bool shouldSkipPath(const QString& candidatePath,
                    const std::vector<QString>& excludedPathPrefixes)
{
    for (const QString& excludedPath : excludedPathPrefixes) {
        if (candidatePath.size() < excludedPath.size()) {
            continue;
        }
        if (candidatePath == excludedPath) {
            return true;
        }
        if (!candidatePath.startsWith(excludedPath)) {
            continue;
        }
        if (excludedPath.endsWith(QLatin1Char('/'))) {
            return true;
        }
        if (candidatePath.at(excludedPath.size()) == QLatin1Char('/')) {
            return true;
        }
    }

    return false;
}

bool isCancelled(const std::shared_ptr<const std::atomic_bool>& cancelFlag)
{
    return cancelFlag && cancelFlag->load(std::memory_order_relaxed);
}

struct LiveWorkerPaths {
    QMutex mutex;
    std::vector<QString> paths;
};

void updateLiveWorkerPath(LiveWorkerPaths* livePaths, size_t index, const QString& path)
{
    if (!livePaths || index >= livePaths->paths.size()) {
        return;
    }

    QMutexLocker locker(&livePaths->mutex);
    livePaths->paths[index] = path;
}

QString currentLiveWorkerPath(LiveWorkerPaths* livePaths)
{
    if (!livePaths) {
        return {};
    }

    QMutexLocker locker(&livePaths->mutex);
    for (auto it = livePaths->paths.rbegin(); it != livePaths->paths.rend(); ++it) {
        if (!it->isEmpty()) {
            return *it;
        }
    }

    return {};
}

size_t previewChildLimitForDepth(int remainingDepth)
{
    if (remainingDepth <= 0) {
        return 0;
    }

    const size_t index = static_cast<size_t>(std::min(
        remainingDepth, static_cast<int>(std::size(kPreviewChildLimits)))) - 1u;
    return kPreviewChildLimits[index];
}

bool pathIsWithinCandidate(const QString& currentPath, const QString& candidatePath)
{
    if (currentPath.size() <= candidatePath.size()) {
        return currentPath == candidatePath;
    }

    if (!currentPath.startsWith(candidatePath)) {
        return false;
    }

    if (candidatePath.endsWith(QLatin1Char('/'))) {
        return true;
    }

    return currentPath.at(candidatePath.size()) == QLatin1Char('/');
}

void addStatsUpwards(FileNode* node, qint64 sizeDelta, int countDelta)
{
    while (node) {
        node->size += sizeDelta;
        node->subtreeFileCount += countDelta;
        node = node->parent;
    }
}

void addSizeUpwards(FileNode* node, qint64 delta)
{
    addStatsUpwards(node, delta, 0);
}

// Recomputes size and subtreeFileCount bottom-up from actual children.
// Used after dynamic task re-parenting to fix sizes without relying on
// incremental addStatsUpwards calls across arena boundaries.
void recomputeNodeStats(FileNode* node)
{
    if (!node || !node->isDirectory())
        return;

    qint64 totalSize = 0;
    int totalFiles = 0;
    for (FileNode* c = node->firstChild; c; c = c->nextSibling) {
        recomputeNodeStats(c);
        totalSize += c->size;
        totalFiles += c->isDirectory() ? c->subtreeFileCount : 1;
    }
    node->size = totalSize;
    node->subtreeFileCount = totalFiles;
}

qint64 recomputeApparentSizes(FileNode* node)
{
    if (!node) {
        return 0;
    }

    if (!node->isDirectory() || node->isVirtual()) {
        node->displaySize = node->isVirtual() ? node->size : std::max<qint64>(node->displaySize, node->size);
        return node->displaySize;
    }

    if (!node->firstChild) {
        // Childless directory: either truly empty (size==0) or depth-truncated
        // clone. In either case, use size as the apparent-size proxy so truncated
        // nodes retain their full subtree weight in the layout.
        if (node->displaySize == 0)
            node->displaySize = node->size;
        return node->displaySize;
    }

    qint64 total = 0;
    for (FileNode* child = node->firstChild; child; child = child->nextSibling) {
        if (!child->isVirtual())
            total += recomputeApparentSizes(child);
    }
    node->displaySize = total;
    return total;
}

FileNode* cloneNodeLimited(const FileNode* node, int remainingDepth,
                           const QString& currentPath, const QString& nodePath,
                           NodeArena& arena, FileNode* parent = nullptr)
{
    if (!node)
        return nullptr;

    FileNode* copy = arena.alloc();
    copy->name = node->name;
    copy->size = node->size;
    copy->displaySize = node->displaySize;
    copy->subtreeFileCount = node->subtreeFileCount;
    copy->setIsDirectory(node->isDirectory());
    copy->setIsVirtual(node->isVirtual());
    copy->color = node->color;
    copy->id = node->id;
    copy->setColorMark(node->colorMark());
    copy->setIconMark(node->iconMark());
    copy->mtime = node->mtime;
    copy->parent = parent;

    if (remainingDepth > 0) {
        const size_t childLimit = previewChildLimitForDepth(remainingDepth);
        const size_t reservedSlots = 1u; // reserve space for activeChild if it exists
        const size_t maxSelected = (childLimit > reservedSlots) ? childLimit - reservedSlots : 0u;

        std::vector<const FileNode*> selectedChildren;
        const FileNode* activeChild = nullptr;
        
        auto compareBySize = [](const FileNode* a, const FileNode* b) {
            return std::max(a->size, a->displaySize) > std::max(b->size, b->displaySize);
        };
        // Min-heap comparison: smallest element at the front.
        auto compareBySmallerSize = [](const FileNode* a, const FileNode* b) {
            return std::max(a->size, a->displaySize) < std::max(b->size, b->displaySize);
        };

        const QString childPathPrefix = childPathPrefixForParent(nodePath);

        for (const FileNode* child = node->firstChild; child; child = child->nextSibling) {
            const qint64 previewSize = std::max(child->size, child->displaySize);
            if (previewSize <= 0 && !child->isDirectory()) {
                continue;
            }
            if (!activeChild && child->isDirectory()) {
                const QString childPath = childPathPrefix + child->name;
                if (pathIsWithinCandidate(currentPath, childPath)) {
                    activeChild = child;
                    continue;
                }
            }

            if (maxSelected > 0) {
                if (selectedChildren.size() < maxSelected) {
                    selectedChildren.push_back(child);
                    if (selectedChildren.size() == maxSelected) {
                        std::make_heap(selectedChildren.begin(), selectedChildren.end(), compareBySmallerSize);
                    }
                } else if (std::max(child->size, child->displaySize) > std::max(selectedChildren.front()->size, selectedChildren.front()->displaySize)) {
                    std::pop_heap(selectedChildren.begin(), selectedChildren.end(), compareBySmallerSize);
                    selectedChildren.back() = child;
                    std::push_heap(selectedChildren.begin(), selectedChildren.end(), compareBySmallerSize);
                }
            }
        }

        std::sort(selectedChildren.begin(), selectedChildren.end(), compareBySize);

        FileNode* lastAdded = nullptr;
        auto appendChild = [&](FileNode* childCopy) {
            if (!copy->firstChild) {
                copy->firstChild = childCopy;
            } else {
                lastAdded->nextSibling = childCopy;
            }
            lastAdded = childCopy;
        };

        if (activeChild) {
            const QString activeChildPath = childPathForParent(nodePath, activeChild->name);
            FileNode* childCopy = cloneNodeLimited(activeChild, remainingDepth - 1, currentPath,
                                                   activeChildPath, arena, copy);
            if (childCopy) {
                appendChild(childCopy);
            }
        }
        for (const FileNode* child : selectedChildren) {
            const QString childPath = childPathForParent(nodePath, child->name);
            FileNode* childCopy = cloneNodeLimited(child, remainingDepth - 1, currentPath,
                                                   childPath, arena, copy);
            if (childCopy)
                appendChild(childCopy);
        }
    }

    return copy;
}

void emitProgress(const ScanResult& scanState, const QString& currentPath,
                  const Scanner::ProgressReadyCallback& progressReadyCallback,
                  const Scanner::ProgressCallback& progressCallback,
                  bool force = false)
{
    if (!progressCallback || !scanState.root)
        return;

    static thread_local QElapsedTimer timer;
    static thread_local bool timerStarted = false;

    if (!timerStarted) {
        timer.start();
        timerStarted = true;
    }

    if (!force && timer.elapsed() < kProgressIntervalMs) {
        return;
    }

    if (!force && progressReadyCallback && !progressReadyCallback()) {
        return;
    }

    timer.restart();

    ScanResult snapshot;
    snapshot.arena = std::make_shared<NodeArena>();
    snapshot.rootPath = scanState.rootPath;
    snapshot.freeBytes = scanState.freeBytes;
    snapshot.totalBytes = scanState.totalBytes;
    snapshot.fileCount = scanState.root->subtreeFileCount;
    snapshot.currentScanPath = currentPath;
    snapshot.root = cloneNodeLimited(scanState.root, kPreviewDepth, currentPath,
                                     scanState.rootPath, *snapshot.arena);
    recomputeApparentSizes(snapshot.root);
    rebuildScanResultSnapshot(snapshot);
    progressCallback(std::move(snapshot));
}

} // namespace
