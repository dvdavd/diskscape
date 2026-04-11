// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#pragma once

#include <QSet>
#include <QString>
#include <cstdint>

// All active filter criteria for the search/filter panel.
// Passed by value; safe to copy and capture in lambdas.
struct FilterParams {
    QString   namePattern;
    qint64    sizeMin = 0;
    qint64    sizeMax = 0;
    int64_t   dateMin = 0;           // mtime seconds since Unix epoch; 0 = no bound
    int64_t   dateMax = 0;
    QSet<QString> fileTypeGroups;    // group names from settings; empty = all types
    bool      filesOnly   = false;
    bool      foldersOnly = false;
    QSet<int> markFilter;            // FolderMark int values; empty = no mark filter
    bool      hideNonMatching = false;

    bool isActive() const {
        return !namePattern.isEmpty() || sizeMin > 0 || sizeMax > 0
            || dateMin != 0 || dateMax != 0 || !fileTypeGroups.isEmpty()
            || filesOnly || foldersOnly || !markFilter.isEmpty();
    }

    bool operator==(const FilterParams& o) const {
        return namePattern == o.namePattern
            && sizeMin == o.sizeMin && sizeMax == o.sizeMax
            && dateMin == o.dateMin && dateMax == o.dateMax
            && fileTypeGroups == o.fileTypeGroups
            && filesOnly == o.filesOnly && foldersOnly == o.foldersOnly
            && markFilter == o.markFilter
            && hideNonMatching == o.hideNonMatching;
    }
    bool operator!=(const FilterParams& o) const { return !(*this == o); }
};
