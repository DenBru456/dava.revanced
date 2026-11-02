#include "QtTools/ProjectInformation/FileSystemCache.h"

#include "Debug/DVAssert.h"
#include "FileSystem/FilePath.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
#include <memory>

using namespace DAVA;

// this function is reimplemented for QSet<QFileInfo> in the class Impl
uint qHash(const QFileInfo& fi, uint seed)
{
    return qHash(fi.absoluteFilePath().toLower(), seed);
}

class FileSystemCache::Impl : public QObject
{
public:
    Impl(const QStringList& supportedExtensions);
    void TrackDirectory(const QString& directory);
    void UntrackDirectory(const QString& directory);
    void UntrackAllDirectories();

    const QStringList& GetTrackedDirectories() const;

    QStringList GetFiles(const QString& extension) const;

private slots:
    void OnDirChanged(const QString& path);
    void OnFileChanged(const QString& path);

private:
    void FlushPendingChanges();
    void RescanDirectory(const QString& directory);
    std::tuple<QStringList, QSet<QFileInfo>> CollectFilesAndDirectories(const QDir& dir) const;
    bool ForceRemovePaths(const QStringList& directories);

    QFileSystemWatcher* watcher = nullptr;
    // a bulk copy fires one directoryChanged per batch; the paths are collected
    // and drained in a single rescan
    QTimer* pendingTimer = nullptr;
    QSet<QString> pendingDirectories;
    QSet<QFileInfo> files;

    QStringList supportedExtensions;
    QStringList directories;
};

FileSystemCache::FileSystemCache(const QStringList& supportedExtensions)
    : impl(new Impl(supportedExtensions))
{
}

FileSystemCache::~FileSystemCache() = default;

void FileSystemCache::TrackDirectory(const QString& directory)
{
    impl->TrackDirectory(directory);
}

void FileSystemCache::UntrackDirectory(const QString& directory)
{
    impl->UntrackDirectory(directory);
}

void FileSystemCache::UntrackAllDirectories()
{
    impl->UntrackAllDirectories();
}

const QStringList& FileSystemCache::GetTrackedDirectories() const
{
    return impl->GetTrackedDirectories();
}

QStringList FileSystemCache::GetFiles(const QString& extension) const
{
    return impl->GetFiles(extension);
}

FileSystemCache::Impl::Impl(const QStringList& supportedExtensions_)
    : QObject(nullptr)
    , watcher(new QFileSystemWatcher(this))
    , pendingTimer(new QTimer(this))
{
    QObject::connect(watcher, &QFileSystemWatcher::fileChanged, this, &Impl::OnFileChanged);
    QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, this, &Impl::OnDirChanged);

    pendingTimer->setSingleShot(true);
    pendingTimer->setInterval(300);
    // no Q_OBJECT in this class: use the functor overload
    QObject::connect(pendingTimer, &QTimer::timeout, this, [this]()
                     { FlushPendingChanges(); });
    for (const QString& extension : supportedExtensions_)
    {
        supportedExtensions << extension.toLower();
    }
}

void FileSystemCache::Impl::TrackDirectory(const QString& directory)
{
    DVASSERT(!directory.isEmpty());
    if (directory.isEmpty())
    {
        return;
    }

    bool alreadyAdded = directories.contains(directory);
    DVASSERT(!alreadyAdded);
    if (alreadyAdded)
    {
        return;
    }

    QFileInfo fileInfo(directory);
    DVASSERT(fileInfo.exists() && fileInfo.isDir());
    DVASSERT(!watcher->directories().contains(directory));
    directories << directory;

    QStringList subDirectories;
    QSet<QFileInfo> filesInDirectory;

    std::tie(subDirectories, filesInDirectory) = CollectFilesAndDirectories(QDir(directory));

    subDirectories << directory;
    watcher->addPaths(subDirectories);
    files += filesInDirectory;
}

void FileSystemCache::Impl::UntrackDirectory(const QString& directory)
{
    DVASSERT(!directory.isEmpty());
    if (directory.isEmpty())
    {
        return;
    }

    bool alreadyAdded = directories.contains(directory);
    DVASSERT(alreadyAdded);
    if (!alreadyAdded)
    {
        return;
    }

    QStringList subDirectories;
    QSet<QFileInfo> filesInDirectory;

    std::tie(subDirectories, filesInDirectory) = CollectFilesAndDirectories(QDir(directory));

    directories.removeOne(directory);
    subDirectories << directory;
    bool removeResult = ForceRemovePaths(subDirectories);
    DVASSERT(removeResult);
    files -= filesInDirectory;
}

void FileSystemCache::Impl::UntrackAllDirectories()
{
    bool removeResult = ForceRemovePaths(watcher->directories());
    DVASSERT(removeResult);
    directories.clear();
    files.clear();
}

const QStringList& FileSystemCache::Impl::GetTrackedDirectories() const
{
    return directories;
}

QStringList FileSystemCache::Impl::GetFiles(const QString& extension) const
{
    QStringList filesList;
    for (const QFileInfo& fileInfo : files)
    {
        if (fileInfo.suffix().toLower() == extension.toLower())
        {
            filesList << fileInfo.absoluteFilePath();
        }
    }
    return filesList;
}

void FileSystemCache::Impl::OnFileChanged(const QString& path)
{
    QFileInfo changedFileInfo(path);
    DVASSERT(changedFileInfo.isFile());
    if (changedFileInfo.exists())
    {
        files.insert(changedFileInfo);
    }
    else
    {
        files.remove(changedFileInfo);
    }
}

void FileSystemCache::Impl::OnDirChanged(const QString& path)
{
    DVASSERT(watcher != nullptr);

    // remember and drain later: FlushPendingChanges runs once per burst
    pendingDirectories.insert(path);
    if (pendingTimer->isActive() == false)
    {
        pendingTimer->start();
    }
}

void FileSystemCache::Impl::FlushPendingChanges()
{
    const QSet<QString> changed = pendingDirectories;
    pendingDirectories.clear();

    for (const QString& directory : changed)
    {
        RescanDirectory(directory);
    }
}

void FileSystemCache::Impl::RescanDirectory(const QString& directory)
{
    QDir changedDir(directory);

    if (changedDir.exists() == false)
    {
        // directory gone: drop its files and its watch
        QMutableSetIterator<QFileInfo> iter(files);
        while (iter.hasNext())
        {
            const QFileInfo fileInfo = iter.next();
            if (fileInfo.absoluteFilePath().startsWith(directory))
            {
                iter.remove();
            }
        }
        watcher->removePath(directory);
        directories.removeAll(directory);
        return;
    }

    QStringList subDirectories;
    QSet<QFileInfo> filesInDirectory;
    std::tie(subDirectories, filesInDirectory) = CollectFilesAndDirectories(changedDir);
    subDirectories << directory;

    // reconcile from the fresh scan instead of stat-ing every cached file
    QMutableSetIterator<QFileInfo> iter(files);
    while (iter.hasNext())
    {
        const QFileInfo fileInfo = iter.next();
        if (fileInfo.absoluteFilePath().startsWith(directory) && filesInDirectory.contains(fileInfo) == false)
        {
            iter.remove();
        }
    }

    const QSet<QString> watched = watcher->directories().toSet();
    QSet<QString> directoriesToAdd;
    for (const QString& subDirectory : subDirectories)
    {
        if (watched.contains(subDirectory) == false)
        {
            directoriesToAdd.insert(subDirectory);
        }
    }
    if (directoriesToAdd.isEmpty() == false)
    {
        watcher->addPaths(directoriesToAdd.values());
    }

    files += filesInDirectory;
}

std::tuple<QStringList, QSet<QFileInfo>> FileSystemCache::Impl::CollectFilesAndDirectories(const QDir& dir) const
{
    DVASSERT(dir.exists());

    QStringList directoriesList;
    QSet<QFileInfo> filesList;
    QString absDirPath(dir.absolutePath());
    QDirIterator dirIterator(absDirPath, QDir::NoDotAndDotDot | QDir::Dirs | QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (dirIterator.hasNext())
    {
        dirIterator.next();
        QFileInfo fileInfo(dirIterator.fileInfo());

        if (fileInfo.isDir())
        {
            directoriesList << fileInfo.absoluteFilePath();
        }

        if (fileInfo.isFile() && supportedExtensions.contains(fileInfo.suffix().toLower()))
        {
            filesList.insert(fileInfo);
        }
    }

    return std::make_tuple(directoriesList, filesList);
}

bool FileSystemCache::Impl::ForceRemovePaths(const QStringList& directories)
{
    QStringList directoriesToRemove = directories;
    while (!directoriesToRemove.empty())
    {
        int countToRemove = directoriesToRemove.size();
        directoriesToRemove = watcher->removePaths(directoriesToRemove);
        int countAfterRemove = directoriesToRemove.size();

        if (countToRemove == countAfterRemove)
        {
            return false;
        }
    }

    return true;
}
