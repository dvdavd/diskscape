// SPDX-FileCopyrightText: 2026 dvdavd
// SPDX-License-Identifier: MIT
#include "filesystemutils.h"

#include <QString>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
    if (matchesAnyFsType(fsType, {"CIFS", "NFS", "SMBFS", "NETFS", "RDPNP"})) {
        return false;
    }

    // Check both device and rootPath for UNC-like starts.
    // Qt might normalize UNC paths to start with // in rootPath().
    if (rootPath.startsWith(QLatin1String("//")) || rootPath.startsWith(QLatin1String("\\\\"))) {
        if (!rootPath.startsWith(QLatin1String("//?/")) && !rootPath.startsWith(QLatin1String("//./")) &&
            !rootPath.startsWith(QLatin1String("\\\\?\\")) && !rootPath.startsWith(QLatin1String("\\\\.\\"))) {
            return false;
        }
    }

    if (device.startsWith("\\\\") || device.startsWith("//")) {
        if (!device.startsWith("\\\\?\\") && !device.startsWith("\\\\.\\") &&
            !device.startsWith("//?/") && !device.startsWith("//./")) {
            return false;
        }
    }

    // Use Win32 API to check for mapped network drives.
    if (rootPath.size() >= 2 && rootPath.at(1) == QLatin1Char(':')) {
        const QString driveRoot = rootPath.left(2).toString() + QLatin1String("\\");
        const UINT type = GetDriveTypeW(reinterpret_cast<const wchar_t*>(driveRoot.utf16()));
        if (type == DRIVE_REMOTE) {
            return false;
        }
    }

    return true;
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
