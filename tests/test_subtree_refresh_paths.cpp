// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filenode.h"
#include "mainwindow_utils.h"
#include "scanner.h"

#include <QtTest/QtTest>

namespace {
ScanResult buildMainTree()
{
    ScanResult result;
    result.arena = std::make_shared<NodeArena>();
    result.rootPath = QStringLiteral("/tmp/root");

    FileNode* root = result.arena->alloc();
    root->name = result.rootPath;
    root->setIsDirectory(true);

    FileNode* child = result.arena->alloc();
    child->name = QStringLiteral("child");
    child->setIsDirectory(true);
    child->parent = root;

    FileNode* leaf = result.arena->alloc();
    leaf->name = QStringLiteral("file.bin");
    leaf->size = 4;
    leaf->parent = child;

    root->firstChild = child;
    child->firstChild = leaf;
    child->size = 4;
    child->subtreeFileCount = 1;
    root->size = 4;
    root->subtreeFileCount = 1;

    result.root = root;
    rebuildScanResultSnapshot(result);
    return result;
}

ScanResult buildRefreshedSubtree()
{
    ScanResult refreshed;
    refreshed.arena = std::make_shared<NodeArena>();
    refreshed.rootPath = QStringLiteral("/tmp/root/child");

    FileNode* root = refreshed.arena->alloc();
    root->name = refreshed.rootPath;
    root->setIsDirectory(true);

    FileNode* leaf = refreshed.arena->alloc();
    leaf->name = QStringLiteral("file.bin");
    leaf->size = 8;
    leaf->parent = root;

    root->firstChild = leaf;
    root->size = 8;
    root->subtreeFileCount = 1;

    refreshed.root = root;
    rebuildScanResultSnapshot(refreshed);
    return refreshed;
}
} // namespace

class TestSubtreeRefreshPaths : public QObject {
    Q_OBJECT

private slots:
    void splicePreservesRelativeNodeName()
    {
        ScanResult main = buildMainTree();
        ScanResult refreshed = buildRefreshedSubtree();

        QVERIFY(spliceRefreshedSubtree(main, QStringLiteral("/tmp/root/child"), std::move(refreshed)));

        FileNode* child = main.root->firstChild;
        QVERIFY(child);
        QCOMPARE(child->name, QStringLiteral("child"));
        QCOMPARE(child->computePath(), QStringLiteral("/tmp/root/child"));
        QVERIFY(findNodeByPath(main.root, QStringLiteral("/tmp/root/child/file.bin")) != nullptr);

        QStringList watchPaths;
        collectWatchDirectoryPaths(main.root, child, watchPaths);
        QVERIFY(watchPaths.contains(QStringLiteral("/tmp/root/child")));
    }
};

QTEST_MAIN(TestSubtreeRefreshPaths)
#include "test_subtree_refresh_paths.moc"
