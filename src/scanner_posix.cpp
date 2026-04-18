// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner_common.h"
#include "colorutils.h"
#include "filesystemutils.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <chrono>
#include <future>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>
#ifdef Q_OS_LINUX
#include <sys/sysmacros.h>
#endif
#include <mutex>
#include <unordered_set>

// (shared scanner helpers are in scanner_common.h)

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

DIR* openDirectoryWithRevalidate(const QByteArray& pathBytes)
{
    DIR* dirp = opendir(pathBytes.constData());
    if (dirp) {
        return dirp;
    }

    const int firstErrno = errno;
    struct stat st;
    if (stat(pathBytes.constData(), &st) == 0 && S_ISDIR(st.st_mode)) {
        dirp = opendir(pathBytes.constData());
        if (dirp) {
            return dirp;
        }
    }

    errno = firstErrno;
    return nullptr;
}

void reportScanWarning(const Scanner::ErrorCallback& errorCallback,
                       const QString& path, int err)
{
    if (!errorCallback || err == 0) {
        return;
    }

    errorCallback({path, QString::fromLocal8Bit(std::strerror(err))});
}


} // namespace

qint64 Scanner::scanNode(FileNode* node, const QString& path, const ScanResult& scanState,
                           const TreemapSettings& settings,
                           const std::vector<QString>& allExcludedPaths,
                           const ProgressReadyCallback& progressReadyCallback,
                           const ProgressCallback& progressCallback, NodeArena& arena,
                           const ActivityCallback& activityCallback,
                           const ErrorCallback& errorCallback,
                           float branchHue, unsigned long long rootDev, std::shared_ptr<const std::atomic_bool> cancelFlag, int depth,
                           bool inMarkedBranch,
                           ScanThrottler* throttler,
                           bool alreadyThrottled)
{
    if (depth > kMaxDepth || isCancelled(cancelFlag)) {
        return 0;
    }

    // Determine current device ID for throttling check.
    // If node is root, we might not have it yet.
    unsigned long long currentDev = rootDev;

    bool needsThrottle = false;
    if (throttler && !alreadyThrottled) {
        // Find current device ID if we are at the top level or crossed a boundary.
        struct stat nodeSt;
        if (stat(path.toLocal8Bit().constData(), &nodeSt) == 0) {
            currentDev = nodeSt.st_dev;
            if (!throttler->isLocal(currentDev, path)) {
                needsThrottle = true;
            }
        }
    }

    ThrottleGuard throttleGuard(throttler ? &throttler->networkSemaphore : nullptr, needsThrottle);
    bool nextAlreadyThrottled = alreadyThrottled || needsThrottle;

    FileNode* lastChild = nullptr;
    if (node->firstChild) {
        lastChild = node->firstChild;
        while (lastChild->nextSibling) lastChild = lastChild->nextSibling;
    }
    if (activityCallback) {
        static thread_local QElapsedTimer activityTimer;
        static thread_local bool activityTimerStarted = false;
        if (!activityTimerStarted) {
            activityTimer.start();
            activityTimerStarted = true;
        }
        if (activityTimer.elapsed() >= kActivityIntervalMs) {
            activityCallback(path, scanState.root ? scanState.root->size : 0);
            activityTimer.restart();
        }
    }

    qint64 totalSize = 0;
    int totalFileCount = 0;

    const QByteArray pathBytes = QFile::encodeName(path);
    const QString childPathPrefix = childPathPrefixForParent(path);
    DIR* dirp = openDirectoryWithRevalidate(pathBytes);
    if (!dirp) {
        reportScanWarning(errorCallback, path, errno);
        return 0;
    }

    const int dfd = dirfd(dirp);
    struct dirent* entry;
    while (true) {
        entry = readdir(dirp);
        if (!entry) {
            break;
        }

        if (isCancelled(cancelFlag)) {
            break;
        }

        const char* dname = entry->d_name;
        if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0')))
            continue;

        unsigned char dtype = entry->d_type;
        if (dtype == DT_LNK)
            continue;

        bool isDir = (dtype == DT_DIR);
        qint64 fileSize = 0;
        qint64 apparentFileSize = 0;
        int64_t fileMtime = 0;
        uint32_t nlink = 1;

#ifdef Q_OS_LINUX
        struct statx stx;
        int statResult = statx(dfd, dname, AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_SIZE | STATX_MTIME | STATX_INO | STATX_NLINK, &stx);

        if (statResult == 0) {
            if (S_ISLNK(stx.stx_mode))
                continue;
            isDir = S_ISDIR(stx.stx_mode);
            if (isDir) {
                if (settings.limitToSameFilesystem && makedev(stx.stx_dev_major, stx.stx_dev_minor) != rootDev)
                    continue;
            } else {
                apparentFileSize = stx.stx_size;
                fileSize = apparentFileSize;
                if (stx.stx_nlink > 1
                        && !shouldCountHardLink(scanState.hardLinkTracker, makedev(stx.stx_dev_major, stx.stx_dev_minor), stx.stx_ino)) {
                    fileSize = 0;
                }
            }
            fileMtime = static_cast<int64_t>(stx.stx_mtime.tv_sec);
            nlink = stx.stx_nlink;
        } else {
            reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
            continue;
        }
#else
        struct stat st;
        int statResult = fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW);

        if (statResult == 0) {
            if (S_ISLNK(st.st_mode))
                continue;
            isDir = S_ISDIR(st.st_mode);
            if (isDir) {
                if (settings.limitToSameFilesystem && st.st_dev != rootDev)
                    continue;
            } else {
                apparentFileSize = st.st_size;
                fileSize = apparentFileSize;
                if (st.st_nlink > 1
                        && !shouldCountHardLink(scanState.hardLinkTracker, st.st_dev, st.st_ino)) {
                    fileSize = 0;
                }
            }
            fileMtime = static_cast<int64_t>(st.st_mtime);
            nlink = st.st_nlink;
        } else {
            reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
            continue;
        }
#endif

        const QString childName = QString::fromLocal8Bit(dname);

        if (isDir) {
            const QString childPath = childPathPrefix + childName;
            if (shouldSkipPath(childPath, allExcludedPaths))
                continue;

            if (activityCallback) {
                static thread_local QElapsedTimer dirActivityTimer;
                static thread_local bool dirActivityTimerStarted = false;
                if (!dirActivityTimerStarted) {
                    dirActivityTimer.start();
                    dirActivityTimerStarted = true;
                }
                if (dirActivityTimer.elapsed() >= kActivityIntervalMs) {
                    activityCallback(childPath, scanState.root ? scanState.root->size : 0);
                    dirActivityTimer.restart();
                }
            }
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(true);
            child->mtime = fileMtime;
            child->parent = node;

            float childBranchHue = branchHue;
            bool childInMarkedBranch = inMarkedBranch;
            if (!settings.folderColorMarks.isEmpty()) {
                auto it = settings.folderColorMarks.constFind(childPath);
                if (it != settings.folderColorMarks.constEnd()) {
                    child->setColorMark(static_cast<uint8_t>(it.value()));
                    childBranchHue = ColorUtils::markHue(static_cast<FolderMark>(child->colorMark()));
                    childInMarkedBranch = true;
                }
            }
            if (!settings.folderIconMarks.isEmpty()) {
                auto it = settings.folderIconMarks.constFind(childPath);
                if (it != settings.folderIconMarks.constEnd()) {
                    child->setIconMark(static_cast<uint8_t>(it.value()));
                }
            }

            if (childInMarkedBranch) {
                child->color = ColorUtils::folderColorForMark(depth + 1, childBranchHue, settings).rgba();
            } else {
                child->color = ColorUtils::folderColor(depth + 1, childBranchHue, settings).rgba();
            }

            appendChild(node, child, &lastChild);

            child->size = scanNode(child, childPath, scanState, settings, allExcludedPaths,
                                   progressReadyCallback, progressCallback, arena, activityCallback,
                                   errorCallback, childBranchHue, rootDev, cancelFlag, depth + 1,
                                   childInMarkedBranch, throttler, nextAlreadyThrottled);
            totalSize += child->size;
            totalFileCount += child->subtreeFileCount;
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPath, progressReadyCallback, progressCallback);
            }
        } else {
            FileNode* child = arena.alloc();
            child->name = childName;
            child->setIsDirectory(false);
            child->parent = node;
            child->size = fileSize;
            child->displaySize = apparentFileSize;
            child->setHasHardLinks(nlink > 1);
            child->mtime = fileMtime;
            child->color = ColorUtils::fileColorForName(childName, settings).rgba();
            child->subtreeFileCount = 1;

            appendChild(node, child, &lastChild);

            totalSize += fileSize;
            totalFileCount += 1;

            if (activityCallback) {
                static thread_local QElapsedTimer activityTimer;
                static thread_local bool activityTimerStarted = false;
                if (!activityTimerStarted) {
                    activityTimer.start();
                    activityTimerStarted = true;
                }
                if (activityTimer.elapsed() >= kActivityIntervalMs) {
                    const QString activityPath = (activityCallback || (depth <= 1 && progressCallback)) ? childPathPrefix + childName : QString();
                    activityCallback(activityPath, fileSize);
                    activityTimer.restart();
                }
            }
            if (depth <= 1 && !isCancelled(cancelFlag)) {
                emitProgress(scanState, childPathPrefix + childName, progressReadyCallback, progressCallback);
            }
        }
    }

    closedir(dirp);
    node->size = totalSize;
    node->subtreeFileCount = totalFileCount;
    return totalSize;
}

ScanResult Scanner::scan(const QString& path, const TreemapSettings& settings,
                         ProgressCallback progressCallback,
                         ProgressReadyCallback progressReadyCallback,
                         ActivityCallback activityCallback,
                         ErrorCallback errorCallback,
                         std::shared_ptr<const std::atomic_bool> cancelFlag)
{
    ScanResult result;
    result.arena = std::make_shared<NodeArena>();

    const QString normalizedPath = QDir::cleanPath(path);
    const QStorageInfo scanStorageInfo(normalizedPath);
    const bool scanIsLocal = isLocalFilesystem(scanStorageInfo);
    if (scanIsLocal) {
        result.hardLinkTracker = std::make_shared<HardLinkTracker>();
    }

    QFileInfo rootInfo(normalizedPath);
    FileNode* root = result.arena->alloc();
    root->name = normalizedPath;
    result.rootPath = normalizedPath;
    root->setIsDirectory(true);
    root->parent = nullptr;
    const float initialHue = ColorUtils::initialFolderBranchHue(root, settings);
    bool rootInMarkedBranch = false;
    if (!settings.folderColorMarks.isEmpty()) {
        auto it = settings.folderColorMarks.constFind(normalizedPath);
        if (it != settings.folderColorMarks.constEnd()) {
            root->setColorMark(static_cast<uint8_t>(it.value()));
            root->color = ColorUtils::folderColorForMark(0, ColorUtils::markHue(static_cast<FolderMark>(root->colorMark())), settings).rgba();
            rootInMarkedBranch = true;
        }
    }
    if (!rootInMarkedBranch) {
        root->color = ColorUtils::folderColor(0, initialHue, settings).rgba();
    }
    if (!settings.folderIconMarks.isEmpty()) {
        auto it = settings.folderIconMarks.constFind(normalizedPath);
        if (it != settings.folderIconMarks.constEnd()) {
            root->setIconMark(static_cast<uint8_t>(it.value()));
        }
    }

    result.root = root;
    const bool trackWorkerPaths = settings.enableScanActivityTracking && static_cast<bool>(progressCallback);
    const bool trackByteActivity = settings.enableScanActivityTracking && static_cast<bool>(activityCallback);
    auto liveBytesSeen = trackByteActivity
        ? std::make_shared<std::atomic<qint64>>(0)
        : std::shared_ptr<std::atomic<qint64>>();
    const int effectiveParallelPartitionDepth = scanIsLocal
        ? settings.parallelPartitionDepth
        : std::min(settings.parallelPartitionDepth, 2);

    const unsigned int concurrencyHint = std::max(1u, std::thread::hardware_concurrency());
    const size_t effectiveWorkerCount = scanIsLocal ? concurrencyHint : std::min(concurrencyHint, 2u);
    const size_t targetTaskCount = effectiveWorkerCount * 4;

    if (!rootInfo.isReadable()) {
        result.freeBytes = scanStorageInfo.bytesFree();
        result.totalBytes = scanStorageInfo.bytesTotal();
        rebuildScanResultSnapshot(result);
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);
    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    }

    std::vector<QString> allExcludedPaths;
    const QStringList defaultExcluded = defaultExcludedPathsForScanRoot(normalizedPath);
    allExcludedPaths.reserve(defaultExcluded.size() + settings.excludedPaths.size());
    for (const QString& excludedPath : defaultExcluded) {
        allExcludedPaths.push_back(QDir::cleanPath(excludedPath));
    }
    for (const QString& excludedPath : settings.excludedPaths) {
        const QString cleanedPath = QDir::cleanPath(excludedPath);
        if (std::find(allExcludedPaths.begin(), allExcludedPaths.end(), cleanedPath) == allExcludedPaths.end()) {
            allExcludedPaths.push_back(cleanedPath);
        }
    }

    struct DirTask {
        FileNode* placeholder = nullptr;
        QString childPath;
        float branchHue = 0.0f;
        unsigned long long rootDev;
        int depth = 0;
        bool inMarkedBranch = false;
    };

    struct PartitionTask {
        FileNode* parent = nullptr;
        QString path;
        float branchHue = 0.0f;
        unsigned long long rootDev;
        int depth = 0;
        bool inMarkedBranch = false;
    };

    struct stat startSt;
    dev_t initialRootDev = 0;
    if (stat(normalizedPath.toLocal8Bit().constData(), &startSt) == 0) {
        initialRootDev = startSt.st_dev;
    }

    auto throttler = std::make_shared<ScanThrottler>();

    std::vector<DirTask> dirTasks;
    std::vector<PartitionTask> partitionQueue;
    partitionQueue.push_back({root, normalizedPath, rootInMarkedBranch ? ColorUtils::markHue(static_cast<FolderMark>(root->colorMark())) : initialHue, static_cast<unsigned long long>(initialRootDev), 0, rootInMarkedBranch});

    for (size_t partitionIndex = 0; partitionIndex < partitionQueue.size(); ++partitionIndex) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        const PartitionTask partition = partitionQueue[partitionIndex];
        const QByteArray pathBytes = QFile::encodeName(partition.path);
        const QString childPathPrefix = childPathPrefixForParent(partition.path);

        bool partitionThrottled = !throttler->isLocal(partition.rootDev, partition.path);
        ThrottleGuard partitionThrottleGuard(&throttler->networkSemaphore, partitionThrottled);

        DIR* dirp = openDirectoryWithRevalidate(pathBytes);
        if (!dirp) {
            reportScanWarning(errorCallback, partition.path, errno);
            continue;
        }

        FileNode* lastPartitionChild = nullptr;
        // Find existing tail if any (though usually empty here)
        if (partition.parent->firstChild) {
            lastPartitionChild = partition.parent->firstChild;
            while (lastPartitionChild->nextSibling) lastPartitionChild = lastPartitionChild->nextSibling;
        }

        const int dfd = dirfd(dirp);
        struct dirent* entry;
        qint64 dirFilesSize = 0;
        int dirFilesCount = 0;

        while (true) {
            entry = readdir(dirp);
            if (!entry) {
                break;
            }

            if (isCancelled(cancelFlag)) {
                break;
            }

            const char* dname = entry->d_name;
            if (dname[0] == '.' && (dname[1] == '\0' || (dname[1] == '.' && dname[2] == '\0'))) {
                continue;
            }

            const unsigned char dtype = entry->d_type;
            if (dtype == DT_LNK) {
                continue;
            }

            bool isDir = (dtype == DT_DIR);
            qint64 fileSize = 0;
            qint64 apparentFileSize = 0;
            int64_t fileMtime = 0;
            uint32_t nlink = 1;

            if (dtype == DT_UNKNOWN || isDir || !isDir) {
#ifdef Q_OS_LINUX
                struct statx stx;
                int statResult = statx(dfd, dname, AT_SYMLINK_NOFOLLOW, STATX_TYPE | STATX_SIZE | STATX_MTIME | STATX_INO | STATX_NLINK, &stx);
                if (statResult == 0) {
                    if (S_ISLNK(stx.stx_mode)) {
                        continue;
                    }
                    isDir = S_ISDIR(stx.stx_mode);
                    if (isDir) {
                        if (settings.limitToSameFilesystem && makedev(stx.stx_dev_major, stx.stx_dev_minor) != partition.rootDev)
                            continue;
                    } else {
                        apparentFileSize = stx.stx_size;
                        fileSize = apparentFileSize;
                        if (stx.stx_nlink > 1
                                && !shouldCountHardLink(result.hardLinkTracker, makedev(stx.stx_dev_major, stx.stx_dev_minor), stx.stx_ino)) {
                            fileSize = 0;
                        }
                    }
                    fileMtime = static_cast<int64_t>(stx.stx_mtime.tv_sec);
                    nlink = stx.stx_nlink;
                } else {
                    reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                    continue;
                }
#else
                struct stat st;
                int statResult = fstatat(dfd, dname, &st, AT_SYMLINK_NOFOLLOW);

                if (statResult == 0) {
                    if (S_ISLNK(st.st_mode)) {
                        continue;
                    }
                    isDir = S_ISDIR(st.st_mode);
                    if (isDir) {
                        if (settings.limitToSameFilesystem && st.st_dev != partition.rootDev)
                            continue;
                    } else {
                        apparentFileSize = st.st_size;
                        fileSize = apparentFileSize;
                        if (st.st_nlink > 1
                                && !shouldCountHardLink(result.hardLinkTracker, st.st_dev, st.st_ino)) {
                            fileSize = 0;
                        }
                    }
                    fileMtime = static_cast<int64_t>(st.st_mtime);
                    nlink = st.st_nlink;
                } else {
                    reportScanWarning(errorCallback, childPathPrefix + QString::fromLocal8Bit(dname), errno);
                    continue;
                }
#endif
            }

            const QString childName = QString::fromLocal8Bit(dname);

            if (isDir) {
                const QString childPath = childPathPrefix + childName;
                if (shouldSkipPath(childPath, allExcludedPaths)) {
                    continue;
                }

                float childBranchHue = partition.branchHue;
                bool childInMarkedBranch = partition.inMarkedBranch;
                if (partition.depth == 0) {
                    childBranchHue = ColorUtils::topLevelFolderBranchHue(childName, settings);
                }

                FileNode* child = result.arena->alloc();
                child->name = childName;
                child->setIsDirectory(true);
                child->mtime = fileMtime;
                child->parent = partition.parent;

                if (!settings.folderColorMarks.isEmpty()) {
                    auto it = settings.folderColorMarks.constFind(childPath);
                    if (it != settings.folderColorMarks.constEnd()) {
                        child->setColorMark(static_cast<uint8_t>(it.value()));
                        childBranchHue = ColorUtils::markHue(static_cast<FolderMark>(child->colorMark()));
                        childInMarkedBranch = true;
                    }
                }
                if (!settings.folderIconMarks.isEmpty()) {
                    auto it = settings.folderIconMarks.constFind(childPath);
                    if (it != settings.folderIconMarks.constEnd()) {
                        child->setIconMark(static_cast<uint8_t>(it.value()));
                    }
                }

                if (childInMarkedBranch) {
                    child->color = ColorUtils::folderColorForMark(partition.depth + 1, childBranchHue, settings).rgba();
                } else {
                    child->color = ColorUtils::folderColor(partition.depth + 1, childBranchHue, settings).rgba();
                }

                appendChild(partition.parent, child, &lastPartitionChild);

                const size_t pendingTasks = dirTasks.size() + (partitionQueue.size() - partitionIndex - 1);
                if (partition.depth + 1 < effectiveParallelPartitionDepth || (pendingTasks < targetTaskCount && partition.depth + 1 < kMaxDepth)) {
                    partitionQueue.push_back({child, childPath, childBranchHue, partition.rootDev, partition.depth + 1, childInMarkedBranch});
                } else {
                    dirTasks.push_back({child, childPath, childBranchHue, partition.rootDev, partition.depth + 1, childInMarkedBranch});
                }
                dirFilesCount += 1;
            } else {
                FileNode* child = result.arena->alloc();
                child->name = childName;
                child->setIsDirectory(false);
                child->parent = partition.parent;
                child->size = fileSize;
                child->displaySize = apparentFileSize;
                child->setHasHardLinks(nlink > 1);
                child->subtreeFileCount = 1;
                child->mtime = fileMtime;
                child->color = ColorUtils::fileColorForName(childName, settings).rgba();

                appendChild(partition.parent, child, &lastPartitionChild);

                dirFilesSize += fileSize;
                dirFilesCount += 1;
                if (trackByteActivity) {
                    liveBytesSeen->fetch_add(fileSize, std::memory_order_relaxed);
                    static thread_local QElapsedTimer activityTimer;
                    static thread_local bool activityTimerStarted = false;
                    if (!activityTimerStarted) {
                        activityTimer.start();
                        activityTimerStarted = true;
                    }
                    if (activityTimer.elapsed() >= kActivityIntervalMs) {
                        activityCallback(childPathPrefix + childName,
                                         liveBytesSeen->load(std::memory_order_relaxed));
                        activityTimer.restart();
                    }
                }
            }
        }

        closedir(dirp);

        if (dirFilesCount > 0) {
            addStatsUpwards(partition.parent, dirFilesSize, dirFilesCount);
        }
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    }

    emitProgress(result, normalizedPath, progressReadyCallback, progressCallback, true);

    struct WorkerResult {
        FileNode* workerRoot = nullptr;
        FileNode* placeholder = nullptr;
    };

    struct CompletedTaskResult {
        std::shared_ptr<NodeArena> arena;
        WorkerResult result;
    };

    // Always launch a full complement of workers.
    const size_t workerCount = dirTasks.empty() ? 0 : effectiveWorkerCount;

    // Heap-allocate shared worker state so threads can safely outlive this function on cancel.
    struct SharedWorkerState {
        std::vector<DirTask> tasks;
        std::atomic_size_t nextTaskIndex{0};
        LiveWorkerPaths liveWorkerPaths;

        std::mutex completedMutex;
        std::vector<CompletedTaskResult> completedTasks;
    };
    auto shared = std::make_shared<SharedWorkerState>();
    shared->tasks = std::move(dirTasks);
    if (trackWorkerPaths) {
        shared->liveWorkerPaths.paths.resize(workerCount);
    }

    std::vector<std::future<void>> futures;
    futures.reserve(workerCount);

    for (size_t workerIndex = 0; workerIndex < workerCount; ++workerIndex) {
        futures.push_back(std::async(std::launch::async,
                                     [shared, throttler, workerIndex, cancelFlag,
                                      trackWorkerPaths, trackByteActivity,
                                      liveBytesSeen, activityCallback, errorCallback,
                                      hardLinkTracker = result.hardLinkTracker,
                                      settings, allExcludedPaths,
                                      arena = result.arena]() {
            while (true) {
                if (isCancelled(cancelFlag))
                    break;

                const size_t taskIndex = shared->nextTaskIndex.fetch_add(1, std::memory_order_relaxed);
                if (taskIndex >= shared->tasks.size()) {
                    break;
                }

                const DirTask& task = shared->tasks[taskIndex];
                WorkerResult r;
                r.placeholder = task.placeholder;
                auto taskArena = std::make_shared<NodeArena>();
                r.workerRoot = taskArena->alloc();
                r.workerRoot->name = task.placeholder->name;
                r.workerRoot->setIsDirectory(true);
                r.workerRoot->parent = nullptr;
                r.workerRoot->color = task.placeholder->color;

                if (trackWorkerPaths)
                    updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, task.childPath);

                std::function<void(const QString&, qint64)> workerActivityCallback;
                if (trackWorkerPaths || trackByteActivity) {
                    workerActivityCallback = [&](const QString& currentPath, qint64 itemBytes) {
                        if (trackWorkerPaths)
                            updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, currentPath);
                        if (!trackByteActivity)
                            return;
                        qint64 totalBytesSeen = liveBytesSeen->load(std::memory_order_relaxed);
                        if (itemBytes > 0)
                            totalBytesSeen = liveBytesSeen->fetch_add(itemBytes, std::memory_order_relaxed) + itemBytes;
                        static thread_local QElapsedTimer activityTimer;
                        static thread_local bool activityTimerStarted = false;
                        if (!activityTimerStarted) { activityTimer.start(); activityTimerStarted = true; }
                        if (activityTimer.elapsed() < kActivityIntervalMs) return;
                        activityTimer.restart();
                        activityCallback(currentPath, totalBytesSeen);
                    };
                }

                ScanResult dummy;
                dummy.root = r.workerRoot;
                dummy.rootPath = task.childPath;
                dummy.hardLinkTracker = hardLinkTracker;

                bool taskThrottled = !throttler->isLocal(task.rootDev, task.childPath);

                Scanner::scanNode(r.workerRoot, task.childPath, dummy, settings, allExcludedPaths, {},
                                  nullptr, *taskArena, workerActivityCallback, errorCallback,
                                  task.branchHue, task.rootDev, cancelFlag, task.depth,
                                  task.inMarkedBranch, throttler.get(), taskThrottled);

                if (isCancelled(cancelFlag))
                    r.workerRoot = nullptr;
                if (trackWorkerPaths)
                    updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, QString());
                {
                    std::lock_guard<std::mutex> lock(shared->completedMutex);
                    shared->completedTasks.push_back({std::move(taskArena), std::move(r)});
                }
            }

            if (trackWorkerPaths)
                updateLiveWorkerPath(&shared->liveWorkerPaths, workerIndex, QString());
        }));
    }

    std::vector<bool> collected(futures.size(), false);
    size_t remaining = futures.size();

    while (remaining > 0) {
        if (isCancelled(cancelFlag)) {
            break;
        }

        bool anyNew = false;
        std::vector<CompletedTaskResult> completed;
        {
            std::lock_guard<std::mutex> lock(shared->completedMutex);
            completed.swap(shared->completedTasks);
        }
        for (CompletedTaskResult& completedTask : completed) {
            WorkerResult& r = completedTask.result;
            if (!r.workerRoot || !r.placeholder) {
                continue;
            }

            const qint64 provisionalSize = r.placeholder->size;
            const int provisionalFileCount = r.placeholder->subtreeFileCount;
            r.placeholder->size = r.workerRoot->size;
            r.placeholder->subtreeFileCount = r.workerRoot->subtreeFileCount;

            r.placeholder->firstChild = r.workerRoot->firstChild;
            for (FileNode* child = r.placeholder->firstChild; child; child = child->nextSibling) {
                child->parent = r.placeholder;
            }

            addStatsUpwards(r.placeholder->parent, r.placeholder->size - provisionalSize,
                            r.placeholder->subtreeFileCount - provisionalFileCount);

            result.arena->merge(std::move(*completedTask.arena));

            anyNew = true;
            result.fileCount = result.root->subtreeFileCount;
            const QString livePath = currentLiveWorkerPath(&shared->liveWorkerPaths);
            emitProgress(result, livePath.isEmpty() ? path : livePath, progressReadyCallback, progressCallback);
        }

        for (size_t i = 0; i < futures.size(); ++i) {
            if (collected[i])
                continue;
            if (futures[i].wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
                continue;

            futures[i].get();
            collected[i] = true;
            --remaining;
            anyNew = true;
        }
        if (!anyNew && remaining > 0) {
            if (trackWorkerPaths) {
                result.fileCount = result.root->subtreeFileCount;
                const QString livePath = currentLiveWorkerPath(&shared->liveWorkerPaths);
                if (!livePath.isEmpty()) {
                    emitProgress(result, livePath, progressReadyCallback, progressCallback);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    }

    if (isCancelled(cancelFlag)) {
        // On cancel: move uncollected futures onto a detached thread so their destructors
        // don't block here. Workers hold only shared_ptr-captured state and will exit
        // quickly once they observe the cancel flag.
        auto abandoned = std::make_shared<std::vector<std::future<void>>>();
        for (size_t i = 0; i < futures.size(); ++i) {
            if (!collected[i]) {
                abandoned->push_back(std::move(futures[i]));
            }
        }
        if (!abandoned->empty()) {
            std::thread([abandoned]() {
                for (auto& f : *abandoned) {
                    f.wait();
                }
            }).detach();
        }
    } else {
        // Normal completion: wait for any stragglers (rare).
        for (size_t i = 0; i < futures.size(); ++i) {
            if (!collected[i]) {
                futures[i].wait();
            }
        }
    }

    if (isCancelled(cancelFlag)) {
        result.root = nullptr;
        rebuildScanResultSnapshot(result);
        return result;
    }

    recomputeApparentSizes(result.root);
    result.fileCount = result.root->subtreeFileCount;

    QStorageInfo storageInfo(path);
    result.freeBytes = storageInfo.bytesFree();
    result.totalBytes = storageInfo.bytesTotal();

    const QString canonicalScanRoot = QFileInfo(normalizedPath).canonicalFilePath();
    const QString primaryFsRoot = QFileInfo(storageInfo.rootPath()).canonicalFilePath();

    // Primary filesystem entry
    QSet<QString> seenDevices;
    seenDevices.insert(spaceSharingDeviceId(storageInfo));
    result.filesystems.push_back({primaryFsRoot, storageInfo.rootPath(),
                                   storageInfo.bytesFree(), storageInfo.bytesTotal(),
                                   isLocalFilesystem(storageInfo)});

    for (const QStorageInfo& vol : QStorageInfo::mountedVolumes()) {
        if (!vol.isValid() || !vol.isReady())
            continue;
        const QString volRoot = QFileInfo(vol.rootPath()).canonicalFilePath();
        if (volRoot == primaryFsRoot)
            continue;
        const bool withinScan = pathIsWithinCandidate(volRoot, canonicalScanRoot);
        if (!withinScan)
            continue;
        // When limited to a single filesystem, don't report other filesystems
        if (settings.limitToSameFilesystem)
            continue;
        const QString cleanVolRoot = QDir::cleanPath(vol.rootPath());
        bool excluded = false;
        for (const QString& excl : allExcludedPaths) {
            if (pathIsWithinCandidate(cleanVolRoot, excl)) {
                excluded = true;
                break;
            }
        }
        if (excluded)
            continue;
        // Skip volumes sharing the same underlying device (e.g. btrfs subvolumes, APFS containers)
        const QString devId = spaceSharingDeviceId(vol);
        if (seenDevices.contains(devId))
            continue;
        seenDevices.insert(devId);
        result.freeBytes += vol.bytesFree();
        result.totalBytes += vol.bytesTotal();
        result.filesystems.push_back({volRoot, vol.rootPath(),
                                       vol.bytesFree(), vol.bytesTotal(),
                                       isLocalFilesystem(vol)});
    }

    emitProgress(result, path, progressReadyCallback, progressCallback, true);

    rebuildScanResultSnapshot(result);
    return result;
}
