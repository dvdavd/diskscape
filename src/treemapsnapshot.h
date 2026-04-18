// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include "filenode.h"

#include <QDir>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <memory>
#include <vector>

struct NodeKey {
    quint64 generation = 0;
    QString normalizedPath;

    bool isValid() const
    {
        return generation != 0 && !normalizedPath.isEmpty();
    }
};

inline bool operator==(const NodeKey& a, const NodeKey& b)
{
    return a.generation == b.generation && a.normalizedPath == b.normalizedPath;
}

struct TreemapSnapshot {
    std::shared_ptr<NodeArena> arena;
    FileNode* root = nullptr;
    QString rootPath;
    quint64 generation = 0;

    FileNode* findNode(const QString& normalizedPath) const
    {
        return findNodeFallback(normalizedPath);
    }

    FileNode* findNodeFallback(const QString& normalizedPath) const
    {
        if (!root || normalizedPath.isEmpty()) return nullptr;

        QString snapshotRootPath = normalizedSnapshotPath(root);
        if (normalizedPath == snapshotRootPath) return root;

        QString prefix = snapshotRootPath;
        if (!prefix.endsWith(QLatin1Char('/'))) {
            prefix += QLatin1Char('/');
        }

        if (!normalizedPath.startsWith(prefix)) {
            return nullptr;
        }

        QString rel = normalizedPath.mid(prefix.length());
        QStringList comps = rel.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        FileNode* curr = root;
        for (const QString& comp : comps) {
            FileNode* next = nullptr;
            for (FileNode* child = curr->firstChild; child; child = child->nextSibling) {
                if (!child->isVirtual() && child->name == comp) {
                    next = child;
                    break;
                }
            }
            if (!next) {
                return nullptr;
            }
            curr = next;
        }
        return curr;
    }

    FileNode* findNode(const NodeKey& key) const
    {
        if (!key.isValid()) {
            return nullptr;
        }
        return findNode(key.normalizedPath);
    }

    NodeKey keyFor(const FileNode* node) const
    {
        if (!node || node->isVirtual() || generation == 0) {
            return {};
        }
        return {generation, normalizedSnapshotPath(node)};
    }

    QString normalizedSnapshotPath(const FileNode* node) const
    {
        if (!node || node->isVirtual()) {
            return {};
        }
        return normalizeSnapshotPath(node->computePath());
    }

    static QString normalizeSnapshotPath(const QString& path)
    {
        QString normalized = QDir::fromNativeSeparators(QDir::cleanPath(path.trimmed()));
#ifdef Q_OS_WIN
        if (normalized.size() == 2 && normalized.at(1) == QLatin1Char(':')) {
            normalized += QLatin1Char('/');
        }
#endif
        return normalized;
    }
};

inline quint64 nextSnapshotGeneration()
{
    static std::atomic<quint64> nextGeneration{1};
    return nextGeneration.fetch_add(1, std::memory_order_relaxed);
}

inline std::shared_ptr<TreemapSnapshot> makeTreemapSnapshot(
    FileNode* root, std::shared_ptr<NodeArena> arena, quint64 generation = nextSnapshotGeneration())
{
    if (!root) {
        return {};
    }

    auto snapshot = std::make_shared<TreemapSnapshot>();
    snapshot->arena = std::move(arena);
    snapshot->root = root;
    snapshot->generation = generation;
    // Note: rootPath should be set by the caller if possible, or it will use root->name
    return snapshot;
}

inline std::shared_ptr<TreemapSnapshot> makeTreemapSnapshot(
    FileNode* root, const QString& rootPath, std::shared_ptr<NodeArena> arena, quint64 generation = nextSnapshotGeneration())
{
    auto snapshot = makeTreemapSnapshot(root, std::move(arena), generation);
    if (snapshot) {
        snapshot->rootPath = rootPath;
    }
    return snapshot;
}
