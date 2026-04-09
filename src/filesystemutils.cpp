// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filesystemutils.h"

#include <QString>

namespace {

bool matchesAnyFsType(const QString& fsType, std::initializer_list<const char*> knownTypes)
{
    for (const char* knownType : knownTypes) {
        if (fsType == QLatin1String(knownType)) {
            return true;
        }
    }
    return false;
}

}

bool isLocalFilesystem(QStringView fileSystemType, const QByteArray& device, QStringView rootPath)
{
    const QString fsType = fileSystemType.trimmed().toString().toUpper();

#ifdef Q_OS_WIN
    if (matchesAnyFsType(fsType, {"CIFS", "NFS", "SMBFS", "NETFS"})) {
        return false;
    }
    return !device.startsWith("\\\\");
#else
    if (matchesAnyFsType(fsType, {"NFS", "NFS4", "CIFS", "SMBFS", "AFPFS",
                                  "WEBDAV", "DAVFS", "9P", "9P2000.L",
                                  "SSHFS", "FUSE.SSHFS"})) {
        return false;
    }
    if (device.startsWith("//")) {
        return false;
    }
    if (rootPath.startsWith(QLatin1String("//"))) {
        return false;
    }
    if (device.startsWith('/')) {
        return true;
    }
    return !device.isEmpty();
#endif
}

bool isLocalFilesystem(const QStorageInfo& storageInfo)
{
    return isLocalFilesystem(QString::fromLatin1(storageInfo.fileSystemType()),
                             storageInfo.device(),
                             storageInfo.rootPath());
}
