#include <Disks/HDFS/DiskHDFS.h>

#include <Storages/HDFS/ReadBufferFromHDFS.h>
#include <Storages/HDFS/WriteBufferFromHDFS.h>
#include "ReadIndirectBufferFromHDFS.h"
#include "WriteIndirectBufferFromHDFS.h"
#include "DiskHDFSReservation.h"
#include "DiskHDFSDirectoryIterator.h"

#include <random>
#include <utility>
#include <memory>
#include <Poco/File.h>
#include <Interpreters/Context.h>
#include <Common/checkStackSize.h>
#include <Common/createHardLink.h>
#include <Common/quoteString.h>
#include <Common/filesystemHelpers.h>
#include <common/logger_useful.h>
#include <Common/thread_local_rng.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int FILE_ALREADY_EXISTS;
    extern const int BAD_ARGUMENTS;
    extern const int CANNOT_DELETE_DIRECTORY;
    extern const int UNKNOWN_FORMAT;
    extern const int PATH_ACCESS_DENIED;
}


DiskHDFS::DiskHDFS(
    const String & disk_name_,
    const String & hdfs_root_path_,
    const String & metadata_path_,
    ContextPtr context_)
    : IDiskRemote(hdfs_root_path_, metadata_path_)
    , log(&Poco::Logger::get("DiskHDFS"))
    , disk_name(disk_name_)
    , config(context_->getGlobalContext()->getConfigRef())
    , hdfs_builder(createHDFSBuilder(hdfs_root_path_, config))
    , hdfs_fs(createHDFSFS(hdfs_builder.get()))
{
}


DiskDirectoryIteratorPtr DiskHDFS::iterateDirectory(const String & path)
{
    return std::make_unique<DiskHDFSDirectoryIterator>(metadata_path + path, path);
}


void DiskHDFS::moveFile(const String & from_path, const String & to_path)
{
    if (exists(to_path))
        throw Exception("File already exists: " + to_path, ErrorCodes::FILE_ALREADY_EXISTS);

    Poco::File(metadata_path + from_path).renameTo(metadata_path + to_path);
}


std::unique_ptr<ReadBufferFromFileBase> DiskHDFS::readFile(const String & path, size_t buf_size, size_t, size_t, size_t, MMappedFileCache *) const
{
    auto metadata = readMeta(path);

    LOG_DEBUG(log,
        "Read from file by path: {}. Existing HDFS objects: {}",
        backQuote(metadata_path + path), metadata.remote_fs_objects.size());

    return std::make_unique<ReadIndirectBufferFromHDFS>(config, remote_fs_root_path, "", metadata, buf_size);
}


std::unique_ptr<WriteBufferFromFileBase> DiskHDFS::writeFile(const String & path, size_t buf_size, WriteMode mode)
{
    bool exist = exists(path);

    if (exist && readMeta(path).read_only)
        throw Exception(ErrorCodes::PATH_ACCESS_DENIED, "File is read-only: " + path);

    /// Path to store new HDFS object.
    auto file_name = getRandomName();
    auto hdfs_path = remote_fs_root_path + file_name;

    if (!exist || mode == WriteMode::Rewrite)
    {
        /// If metadata file exists - remove and new.
        if (exist)
            removeFile(path);

        auto metadata = createMeta(path);
        /// Save empty metadata to disk to have ability to get file size while buffer is not finalized.
        metadata.save();

        LOG_DEBUG(log,
            "Write to file by path: {}. New hdfs path: {}", backQuote(metadata_path + path), hdfs_path);

        return std::make_unique<WriteIndirectBufferFromHDFS>(config, hdfs_path, file_name, metadata, buf_size);
    }
    else
    {
        auto metadata = readMeta(path);

        LOG_DEBUG(log,
            "Append to file by path: {}. New hdfs path: {}. Existing HDFS objects: {}",
            backQuote(metadata_path + path), hdfs_path, metadata.remote_fs_objects.size());

        return std::make_unique<WriteIndirectBufferFromHDFS>(config, hdfs_path, file_name, metadata, buf_size);
    }
}


void DiskHDFS::removeFile(const String & path)
{
    LOG_DEBUG(&Poco::Logger::get("DiskHDFS"), "Remove file by path: {}", backQuote(metadata_path + path));

    Poco::File file(metadata_path + path);

    if (!file.isFile())
        throw Exception(ErrorCodes::CANNOT_DELETE_DIRECTORY, "Path '{}' is a directory", path);

    try
    {
        auto metadata = readMeta(path);

        /// If there is no references - delete content from HDFS.
        if (metadata.ref_count == 0)
        {
            file.remove();

            for (const auto & [hdfs_object_path, _] : metadata.remote_fs_objects)
            {
                const size_t begin_of_path = remote_fs_root_path.find('/', remote_fs_root_path.find("//") + 2);
                const String hdfs_path = remote_fs_root_path.substr(begin_of_path) + hdfs_object_path;

                int res = hdfsDelete(hdfs_fs.get(), hdfs_path.c_str(), 0);
                if (res == -1)
                    throw Exception(ErrorCodes::LOGICAL_ERROR, "HDFSDelete failed with path: " + hdfs_path);
            }
        }
        else /// In other case decrement number of references, save metadata and delete file.
        {
            --metadata.ref_count;
            metadata.save();
            file.remove();
        }
    }
    catch (const Exception & e)
    {
        /// If it's impossible to read meta - just remove it from FS.
        if (e.code() == ErrorCodes::UNKNOWN_FORMAT)
        {
            LOG_WARNING(
                log,
                "Metadata file {} can't be read by reason: {}. Removing it forcibly.",
                backQuote(path),
                e.nested() ? e.nested()->message() : e.message());

            file.remove();
        }
        else
            throw;
    }
}


void DiskHDFS::removeFileIfExists(const String & path)
{
    int exists_status = hdfsExists(hdfs_fs.get(), path.data());
    if (exists_status == 0)
        removeFile(path);
    else
        LOG_DEBUG(&Poco::Logger::get("DiskHDFS"), "File by path {} does not exist", backQuote(metadata_path + path));
}


void DiskHDFS::removeRecursive(const String & path)
{
    checkStackSize(); /// This is needed to prevent stack overflow in case of cyclic symlinks.

    Poco::File file(metadata_path + path);
    if (file.isFile())
    {
        removeFile(path);
    }
    else
    {
        for (auto it{iterateDirectory(path)}; it->isValid(); it->next())
            removeRecursive(it->path());
        file.remove();
    }
}


void DiskHDFS::createHardLink(const String & src_path, const String & dst_path)
{
    /// Increment number of references.
    auto src = readMeta(src_path);
    ++src.ref_count;
    src.save();

    /// Create FS hardlink to metadata file.
    DB::createHardLink(metadata_path + src_path, metadata_path + dst_path);
}


ReservationPtr DiskHDFS::reserve(UInt64 bytes)
{
    if (!tryReserve(bytes))
        return {};
    return std::make_unique<DiskHDFSReservation>(std::static_pointer_cast<DiskHDFS>(shared_from_this()), bytes);
}


bool DiskHDFS::tryReserve(UInt64 bytes)
{
    std::lock_guard lock(reservation_mutex);

    if (bytes == 0)
    {
        LOG_DEBUG(log, "Reserving 0 bytes on HDFS disk {}", backQuote(disk_name));
        ++reservation_count;
        return true;
    }

    auto available_space = getAvailableSpace();
    UInt64 unreserved_space = available_space - std::min(available_space, reserved_bytes);
    if (unreserved_space >= bytes)
    {
        LOG_DEBUG(log,
            "Reserving {} on disk {}, having unreserved {}",
            formatReadableSizeWithBinarySuffix(bytes), backQuote(disk_name), formatReadableSizeWithBinarySuffix(unreserved_space));
        ++reservation_count;
        reserved_bytes += bytes;
        return true;
    }

    return false;
}


void registerDiskHDFS(DiskFactory & factory)
{
    auto creator = [](const String & name,
                      const Poco::Util::AbstractConfiguration & config,
                      const String & config_prefix,
                      ContextConstPtr context_) -> DiskPtr
    {
        Poco::File disk{context_->getPath() + "disks/" + name};
        disk.createDirectories();

        String uri{config.getString(config_prefix + ".endpoint")};

        if (uri.back() != '/')
            throw Exception(ErrorCodes::BAD_ARGUMENTS, "HDFS path must ends with '/', but '{}' doesn't.", uri);

        String metadata_path = context_->getPath() + "disks/" + name + "/";
        auto context = Context::createCopy(context_);

        return std::make_shared<DiskHDFS>(name, uri, metadata_path, context);
    };

    factory.registerDiskType("hdfs", creator);
}

}
