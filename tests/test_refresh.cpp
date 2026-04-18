// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filenode.h"
#include "mainwindow_utils.h"
#include "scanner.h"

#include <QApplication>
#include <QtTest/QtTest>

// Builds a minimal two-level ScanResult:
//   root (/home/user, 1000)
//   ├── sub  (dir, 400)
//   └── file.txt (file, 600)
static ScanResult buildMainTree()
{
    auto arena = std::make_shared<NodeArena>();
    ScanResult r;
    r.arena = arena;
    r.rootPath = "/home/user";

    FileNode* root = arena->alloc();
    root->name = r.rootPath;
    root->setIsDirectory(true);
    root->size = 1000;

    FileNode* sub = arena->alloc();
    sub->name = "sub";
    sub->setIsDirectory(true);
    sub->size = 400;
    sub->parent = root;

    FileNode* file = arena->alloc();
    file->name = "file.txt";
    file->size = 600;
    file->parent = root;

    root->firstChild = sub;
    sub->nextSibling = file;

    r.root = root;
    rebuildScanResultSnapshot(r);
    return r;
}

static ScanResult buildNestedTree()
{
    ScanResult r = buildMainTree();
    auto* sub = r.root->firstChild;
    FileNode* nested = r.arena->alloc();
    nested->name = "nested";
    nested->setIsDirectory(true);
    nested->size = 250;
    nested->parent = sub;
    nested->nextSibling = sub->firstChild;
    sub->firstChild = nested;
    sub->subtreeFileCount = 1;
    sub->size = 250;
    r.root->size = 850;
    rebuildScanResultSnapshot(r);
    return r;
}

class TestRefresh : public QObject {
    Q_OBJECT

private slots:
    void splice_replacesChildInParent()
    {
        ScanResult main = buildMainTree();
        FileNode* oldSub = main.root->firstChild; // the "sub" node

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 800;
        refreshed.root = newSub;

        QVERIFY(spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed)));

        QCOMPARE(main.root->firstChild, newSub);
        QVERIFY(main.root->firstChild != oldSub);
    }

    void splice_setsParentPointer()
    {
        ScanResult main = buildMainTree();

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 400;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(newSub->parent, main.root);
    }

    void splice_propagatesSizeDeltaUpward()
    {
        ScanResult main = buildMainTree();
        // sub was 400, new is 800 → delta = +400 → root should go from 1000 to 1400

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 800;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(main.root->size, qint64(1400));
    }

    void splice_negativeDeltaPropagatesCorrectly()
    {
        ScanResult main = buildMainTree();
        // sub was 400, new is 100 → delta = -300 → root should go from 1000 to 700

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 100;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QCOMPARE(main.root->size, qint64(700));
    }

    void splice_mergesArenas()
    {
        ScanResult main = buildMainTree();
        const size_t before = main.arena->totalAllocated();

        auto refreshArena = std::make_shared<NodeArena>();
        // Allocate a few extra nodes in the refresh arena
        for (int i = 0; i < 5; ++i) refreshArena->alloc();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshed.arena->alloc();
        newSub->setIsDirectory(true);
        newSub->size = 400;
        refreshed.root = newSub;

        spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed));

        QVERIFY(main.arena->totalAllocated() > before);
    }

    void splice_returnsFalseForUnknownPath()
    {
        ScanResult main = buildMainTree();

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/nonexistent";
        FileNode* newNode = refreshArena->alloc();
        newNode->setIsDirectory(true);
        newNode->size = 100;
        refreshed.root = newNode;

        QVERIFY(!spliceRefreshedSubtree(main, "/home/user/nonexistent", std::move(refreshed)));
    }

    void splice_returnsFalseForRootNode()
    {
        ScanResult main = buildMainTree();
        // The root itself has no parent — splice should refuse it

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user";
        FileNode* newRoot = refreshArena->alloc();
        newRoot->setIsDirectory(true);
        newRoot->size = 2000;
        refreshed.root = newRoot;

        QVERIFY(!spliceRefreshedSubtree(main, "/home/user", std::move(refreshed)));
        // Original tree unchanged
        QCOMPARE(main.root->size, qint64(1000));
    }

    void splice_updatesFilesystemTotalsFromRefresh()
    {
        ScanResult main = buildMainTree();
        main.filesystems = {
            {QStringLiteral("/"), QStringLiteral("/"), 100, 500, true},
            {QStringLiteral("/mnt/data"), QStringLiteral("/mnt/data"), 200, 1000, true},
        };
        main.freeBytes = 300;
        main.totalBytes = 1500;

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 400;
        refreshed.root = newSub;
        refreshed.filesystems = {
            {QStringLiteral("/"), QStringLiteral("/"), 150, 550, true},
            {QStringLiteral("/media/usb"), QStringLiteral("/media/usb"), 50, 64, true},
        };

        QVERIFY(spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed)));

        QCOMPARE(main.filesystems.size(), 3);
        QCOMPARE(main.freeBytes, qint64(400));
        QCOMPARE(main.totalBytes, qint64(1614));
    }

    void snapshot_lookup_resolvesByPath()
    {
        ScanResult main = buildMainTree();
        QVERIFY(main.snapshot);
        QCOMPARE(findNodeByPath(main.snapshot, "/home/user"), main.root);
        QCOMPARE(findNodeByPath(main.snapshot, "/home/user/sub"), main.root->firstChild);
        QCOMPARE(findNodeByPath(main.snapshot, "/home/user/missing"), nullptr);
    }

    void viewState_remap_usesSnapshotIndexAfterSubtreeRefresh()
    {
        ScanResult main = buildMainTree();
        FileNode* oldSub = main.root->firstChild;

        TreemapWidget::ViewState view;
        view.nodeKey = main.snapshot->keyFor(oldSub);
        view.cameraScale = 2.0;

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 800;
        refreshed.root = newSub;

        QVERIFY(spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed)));
        rebuildScanResultSnapshot(main);
        QVERIFY(main.snapshot);

        FileNode* resolved = main.snapshot->findNode(view.nodeKey);
        QCOMPARE(resolved, newSub);
        QCOMPARE(view.cameraScale, 2.0);
    }

    void viewState_remap_fallsBackToNearestExistingAncestor()
    {
        ScanResult main = buildNestedTree();
        FileNode* sub = main.root->firstChild;
        FileNode* nested = sub->firstChild;

        TreemapWidget::ViewState view;
        view.nodeKey = main.snapshot->keyFor(nested);

        auto refreshArena = std::make_shared<NodeArena>();
        ScanResult refreshed;
        refreshed.arena = refreshArena;
        refreshed.rootPath = "/home/user/sub";
        FileNode* newSub = refreshArena->alloc();
        newSub->name = "sub";
        newSub->setIsDirectory(true);
        newSub->size = 100;
        refreshed.root = newSub;

        QVERIFY(spliceRefreshedSubtree(main, "/home/user/sub", std::move(refreshed)));
        rebuildScanResultSnapshot(main);

        FileNode* resolved = main.snapshot->findNode(view.nodeKey);
        if (!resolved) {
            resolved = main.snapshot->findNode(nearestExistingNodeKey(main.snapshot, view.nodeKey));
        }
        QCOMPARE(resolved, newSub);
    }
};

QTEST_MAIN(TestRefresh)
#include "test_refresh.moc"
