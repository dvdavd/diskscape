// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QDir>
#include <QString>
#include <QRgb>
#include <QRectF>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <vector>
#include <new>
#include <cstdlib>

struct NodeRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;

    NodeRect() = default;
    NodeRect(const QRectF& rect)
        : x(static_cast<float>(rect.x()))
        , y(static_cast<float>(rect.y()))
        , w(static_cast<float>(rect.width()))
        , h(static_cast<float>(rect.height()))
    {
    }

    NodeRect& operator=(const QRectF& rect)
    {
        x = static_cast<float>(rect.x());
        y = static_cast<float>(rect.y());
        w = static_cast<float>(rect.width());
        h = static_cast<float>(rect.height());
        return *this;
    }

    operator QRectF() const
    {
        return QRectF(x, y, w, h);
    }

    bool isEmpty() const
    {
        return w <= 0.0f || h <= 0.0f;
    }

    qreal width() const { return w; }
    qreal height() const { return h; }
    qreal left() const { return x; }
    qreal top() const { return y; }
};

struct FileNode {
    QString name;               // 8 bytes
    FileNode* parent = nullptr; // 8 bytes
    FileNode* firstChild = nullptr; // 8 bytes
    FileNode* nextSibling = nullptr; // 8 bytes
    qint64 size = 0;            // 8 bytes
    qint64 displaySize = 0;     // 8 bytes
    int64_t mtime = 0;          // 8 bytes
    QRgb color = 0;             // 4 bytes
    uint32_t id = UINT32_MAX;   // 4 bytes
    int subtreeFileCount = 0;   // 4 bytes
    
    // Combined marks and flags:
    // bits 0-7:   colorMark
    // bits 8-15:  iconMark
    // bit 16:     isDirectory
    // bit 17:     isVirtual
    // bit 18:     hasHardLinks
    uint32_t flags = 0;         // 4 bytes

    // Total: 72 bytes.

    bool isDirectory() const { return (flags >> 16) & 1; }
    void setIsDirectory(bool v) { if (v) flags |= (1 << 16); else flags &= ~(1 << 16); }

    bool isVirtual() const { return (flags >> 17) & 1; }
    void setIsVirtual(bool v) { if (v) flags |= (1 << 17); else flags &= ~(1 << 17); }

    bool hasHardLinks() const { return (flags >> 18) & 1; }
    void setHasHardLinks(bool v) { if (v) flags |= (1 << 18); else flags &= ~(1 << 18); }

    uint8_t colorMark() const { return static_cast<uint8_t>(flags & 0xFF); }
    void setColorMark(uint8_t v) { flags = (flags & ~0xFFu) | v; }

    uint8_t iconMark() const { return static_cast<uint8_t>((flags >> 8) & 0xFF); }
    void setIconMark(uint8_t v) { flags = (flags & ~0xFF00u) | (static_cast<uint32_t>(v) << 8); }

    // Returns the full absolute path.
    // Descendants reconstruct it from the parent chain to avoid per-node path storage.
    // This relies on the root node (parent == nullptr) having its absolute path
    // stored in its 'name' field.
    QString computePath() const
    {
        if (!parent) {
            return name;
        }

        // Collect the chain of nodes from this node up to the root.
        std::vector<const FileNode*> chain;
        for (const FileNode* curr = this; curr; curr = curr->parent) {
            chain.push_back(curr);
        }

        // Build the path starting from the root (last element in the chain).
        QString res = chain.back()->name;
        for (auto it = chain.rbegin() + 1; it != chain.rend(); ++it) {
            const QString& childName = (*it)->name;
            if (!res.endsWith(QLatin1Char('/'))) {
                res += QLatin1Char('/');
            }
            res += childName;
        }
        return res;
    }
};

// Appends child to the end of parent's child list and sets child->parent.
// If lastChildHint is provided, it is assumed to be the current tail of the parent's child list.
inline void appendChild(FileNode* parent, FileNode* child, FileNode** lastChildHint = nullptr)
{
    if (!parent || !child) return;
    child->parent = parent;
    child->nextSibling = nullptr;

    if (lastChildHint && *lastChildHint) {
        (*lastChildHint)->nextSibling = child;
        *lastChildHint = child;
        return;
    }

    if (!parent->firstChild) {
        parent->firstChild = child;
        if (lastChildHint) *lastChildHint = child;
    } else {
        FileNode* last = parent->firstChild;
        while (last->nextSibling) {
            last = last->nextSibling;
        }
        last->nextSibling = child;
        if (lastChildHint) *lastChildHint = child;
    }
}

struct FileNodeStats {
    int fileCount = 0;
    qint64 totalSize = 0;
};

// Bump/arena allocator for FileNode.
// Allocates nodes in large contiguous chunks, eliminating per-node malloc
// overhead and improving cache locality.
// Optimized to only construct and destruct nodes that are actually used.
class NodeArena {
    static constexpr size_t kChunkNodes = 4096; // ~300KB chunks (72 bytes * 4k)

    struct Chunk {
        FileNode* nodes;
        size_t used;
    };

    std::vector<Chunk> m_chunks;

public:
    NodeArena() = default;

    ~NodeArena()
    {
        for (const auto& chunk : m_chunks) {
            for (size_t i = 0; i < chunk.used; ++i) {
                chunk.nodes[i].~FileNode();
            }
            std::free(chunk.nodes);
        }
    }

    NodeArena(const NodeArena&) = delete;
    NodeArena& operator=(const NodeArena&) = delete;

    FileNode* alloc()
    {
        if (m_chunks.empty() || m_chunks.back().used == kChunkNodes) {
            void* ptr = std::malloc(kChunkNodes * sizeof(FileNode));
            if (!ptr) return nullptr;
            m_chunks.push_back({static_cast<FileNode*>(ptr), 0});
        }
        FileNode* res = &m_chunks.back().nodes[m_chunks.back().used++];
        new (res) FileNode();
        return res;
    }

    // Consume all chunks from other, appending them to this arena.
    void merge(NodeArena&& other)
    {
        for (const auto& chunk : other.m_chunks) {
            m_chunks.push_back(chunk);
        }
        other.m_chunks.clear();
    }

    size_t totalAllocated() const
    {
        size_t total = 0;
        for (const auto& chunk : m_chunks) {
            total += chunk.used;
        }
        return total;
    }
};
