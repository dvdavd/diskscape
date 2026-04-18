// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "scanner.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStringList>
#include <QTextStream>

namespace {

struct TreeStats {
    qint64 nodes = 0;
    qint64 directories = 0;
    qint64 files = 0;
    qint64 maxDepth = 0;
};

void collectTreeStats(const FileNode* node, qint64 depth, TreeStats& stats)
{
    if (!node) {
        return;
    }

    ++stats.nodes;
    stats.maxDepth = std::max(stats.maxDepth, depth);
    if (node->isDirectory()) {
        ++stats.directories;
    } else {
        ++stats.files;
    }

    for (const FileNode* child = node->firstChild; child; child = child->nextSibling) {
        collectTreeStats(child, depth + 1, stats);
    }
}

QString formatMs(qint64 value)
{
    return QString::number(static_cast<double>(value) / 1000.0, 'f', 3);
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    QTextStream out(stdout);
    QTextStream err(stderr);

    QStringList args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args.push_back(QString::fromLocal8Bit(argv[i]));
    }
    if (args.size() < 2) {
        err << "Usage: scanner_bench <path> [--iterations N] [--partition-depth N] [--no-activity-tracking]\n";
        return 1;
    }

    const QString path = QFileInfo(args.at(1)).absoluteFilePath();
    bool activityTracking = true;
    int iterations = 1;
    int partitionDepth = TreemapSettings::defaults().parallelPartitionDepth;
    for (int i = 2; i < args.size(); ++i) {
        const QString& arg = args.at(i);
        if (arg == QLatin1String("--iterations")) {
            if (i + 1 >= args.size()) {
                err << "Missing value for --iterations\n";
                return 1;
            }
            iterations = args.at(++i).toInt();
            continue;
        }
        if (arg == QLatin1String("--partition-depth")) {
            if (i + 1 >= args.size()) {
                err << "Missing value for --partition-depth\n";
                return 1;
            }
            partitionDepth = args.at(++i).toInt();
            continue;
        }
        if (arg == QLatin1String("--no-activity-tracking")) {
            activityTracking = false;
            continue;
        }

        err << "Unknown argument: " << arg << '\n';
        return 1;
    }

    if (iterations < 1) {
        err << "Iterations must be >= 1\n";
        return 1;
    }

    if (!QFileInfo::exists(path)) {
        err << "Path does not exist: " << QDir::toNativeSeparators(path) << '\n';
        return 1;
    }

    TreemapSettings settings = TreemapSettings::defaults();
    settings.enableScanActivityTracking = activityTracking;
    settings.parallelPartitionDepth = partitionDepth;
    settings.sanitize();

    qint64 measuredElapsedSumMs = 0;
    qint64 measuredIterations = 0;
    TreeStats stats;
    qint64 totalBytes = 0;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        QElapsedTimer totalTimer;
        totalTimer.start();
        ScanResult result = Scanner::scan(path, settings);
        const qint64 elapsedMs = totalTimer.elapsed();
        out << "iteration_" << (iteration + 1) << "_s: " << formatMs(elapsedMs) << '\n';
        if (iteration > 0 || iterations == 1) {
            measuredElapsedSumMs += elapsedMs;
            ++measuredIterations;
        }

        if (!result.root) {
            err << "Scan failed or was cancelled.\n";
            return 2;
        }

        if (iteration == iterations - 1) {
            collectTreeStats(result.root, 0, stats);
            totalBytes = result.root->size;
        }
    }

    out << "path: " << QDir::toNativeSeparators(path) << '\n';
    out << "iterations: " << iterations << '\n';
    out << "warmup_iterations: " << (iterations > 1 ? 1 : 0) << '\n';
    out << "measured_iterations: " << measuredIterations << '\n';
    out << "activity_tracking: " << (settings.enableScanActivityTracking ? "on" : "off") << '\n';
    out << "partition_depth: " << settings.parallelPartitionDepth << '\n';
    out << "elapsed_avg_s: " << formatMs(measuredElapsedSumMs / std::max<qint64>(1, measuredIterations)) << '\n';
    out << "nodes: " << stats.nodes << '\n';
    out << "directories: " << stats.directories << '\n';
    out << "files: " << stats.files << '\n';
    out << "max_depth: " << stats.maxDepth << '\n';
    out << "bytes: " << totalBytes << '\n';

    return 0;
}
