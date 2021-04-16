#include <Disks/IDiskRemote.h>

#include "Disks/DiskFactory.h"

#include <random>
#include <utility>
#include <optional>
#include <IO/ReadBufferFromFile.h>
#include <IO/ReadBufferFromS3.h>
#include <IO/ReadHelpers.h>
#include <IO/SeekAvoidingReadBuffer.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromS3.h>
#include <IO/WriteHelpers.h>
#include <Poco/File.h>
#include <Common/checkStackSize.h>
#include <Common/createHardLink.h>
#include <Common/quoteString.h>
#include <Common/thread_local_rng.h>
#include <Common/ThreadPool.h>
#include <common/logger_useful.h>
#include <boost/algorithm/string.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNKNOWN_FORMAT;
}


/// Load metadata by path or create empty if `create` flag is set.
IDiskRemote::Metadata::Metadata(
        const String & remote_fs_root_path_,
        const String & disk_path_,
        const String & metadata_file_path_,
        bool create)
    : remote_fs_root_path(remote_fs_root_path_)
    , disk_path(disk_path_)
    , metadata_file_path(metadata_file_path_)
    , total_size(0), remote_fs_objects(0), ref_count(0)
{
    if (create)
        return;

    try
    {
        ReadBufferFromFile buf(disk_path + metadata_file_path, 1024); /* reasonable buffer size for small file */

        UInt32 version;
        readIntText(version, buf);
        assertChar('\n', buf);

        if (version < VERSION_ABSOLUTE_PATHS || version > VERSION_READ_ONLY_FLAG)
            throw Exception(
                ErrorCodes::UNKNOWN_FORMAT,
                "Unknown metadata file version. Path: {}. Version: {}. Maximum expected version: {}",
                disk_path + metadata_file_path, std::to_string(version), std::to_string(VERSION_READ_ONLY_FLAG));

        UInt32 remote_fs_objects_count;
        readIntText(remote_fs_objects_count, buf);
        assertChar('\t', buf);
        readIntText(total_size, buf);
        assertChar('\n', buf);
        remote_fs_objects.resize(remote_fs_objects_count);

        for (size_t i = 0; i < remote_fs_objects_count; ++i)
        {
            String remote_fs_object_path;
            size_t remote_fs_object_size;
            readIntText(remote_fs_object_size, buf);
            assertChar('\t', buf);
            readEscapedString(remote_fs_object_path, buf);
            if (version == VERSION_ABSOLUTE_PATHS)
            {
                if (!boost::algorithm::starts_with(remote_fs_object_path, remote_fs_root_path))
                    throw Exception(
                        ErrorCodes::UNKNOWN_FORMAT,
                        "Path in metadata does not correspond S3 root path. Path: {}, root path: {}, disk path: {}",
                        remote_fs_object_path, remote_fs_root_path, disk_path_);

                remote_fs_object_path = remote_fs_object_path.substr(remote_fs_root_path.size());
            }
            assertChar('\n', buf);
            remote_fs_objects[i] = {remote_fs_object_path, remote_fs_object_size};
        }

        readIntText(ref_count, buf);
        assertChar('\n', buf);

        if (version >= VERSION_READ_ONLY_FLAG)
        {
            readBoolText(read_only, buf);
            assertChar('\n', buf);
        }
    }
    catch (Exception & e)
    {
        if (e.code() == ErrorCodes::UNKNOWN_FORMAT)
            throw;

        throw Exception("Failed to read metadata file", e, ErrorCodes::UNKNOWN_FORMAT);
    }
}

void IDiskRemote::Metadata::addObject(const String & path, size_t size)
{
    total_size += size;
    remote_fs_objects.emplace_back(path, size);
}

/// Fsync metadata file if 'sync' flag is set.
void IDiskRemote::Metadata::save(bool sync)
{
    WriteBufferFromFile buf(disk_path + metadata_file_path, 1024);

    writeIntText(VERSION_RELATIVE_PATHS, buf);
    writeChar('\n', buf);

    writeIntText(remote_fs_objects.size(), buf);
    writeChar('\t', buf);
    writeIntText(total_size, buf);
    writeChar('\n', buf);

    for (const auto & [remote_fs_object_path, remote_fs_object_size] : remote_fs_objects)
    {
        writeIntText(remote_fs_object_size, buf);
        writeChar('\t', buf);
        writeEscapedString(remote_fs_object_path, buf);
        writeChar('\n', buf);
    }

    writeIntText(ref_count, buf);
    writeChar('\n', buf);

    writeBoolText(read_only, buf);
    writeChar('\n', buf);

    buf.finalize();
    if (sync)
        buf.sync();
}


IDiskRemote::Metadata IDiskRemote::readMeta(const String & path) const
{
    return Metadata(remote_fs_root_path, metadata_path, path);
}

IDiskRemote::Metadata IDiskRemote::createMeta(const String & path) const
{
    return Metadata(remote_fs_root_path, metadata_path, path, true);
}


IDiskRemote::IDiskRemote(
    const String & remote_fs_root_path_,
    const String & metadata_path_,
    std::unique_ptr<Executor> executor_)
    : IDisk(std::move(executor_))
    , remote_fs_root_path(remote_fs_root_path_)
    , metadata_path(metadata_path_)
{
}


bool IDiskRemote::exists(const String & path) const
{
    return Poco::File(metadata_path + path).exists();
}


bool IDiskRemote::isFile(const String & path) const
{
    return Poco::File(metadata_path + path).isFile();
}


void IDiskRemote::createFile(const String & path)
{
    /// Create empty metadata file.
    auto metadata = createMeta(path);
    metadata.save();
}


size_t IDiskRemote::getFileSize(const String & path) const
{
    auto metadata = readMeta(path);
    return metadata.total_size;
}


void IDiskRemote::replaceFile(const String & from_path, const String & to_path)
{
    if (exists(to_path))
    {
        const String tmp_path = to_path + ".old";
        moveFile(to_path, tmp_path);
        moveFile(from_path, to_path);
        removeFile(tmp_path);
    }
    else
        moveFile(from_path, to_path);
}


void IDiskRemote::setReadOnly(const String & path)
{
    /// We should store read only flag inside metadata file (instead of using FS flag),
    /// because we modify metadata file when create hard-links from it.
    auto metadata = readMeta(path);
    metadata.read_only = true;
    metadata.save();
}


bool IDiskRemote::isDirectory(const String & path) const
{
    return Poco::File(metadata_path + path).isDirectory();
}


void IDiskRemote::createDirectory(const String & path)
{
    Poco::File(metadata_path + path).createDirectory();
}


void IDiskRemote::createDirectories(const String & path)
{
    Poco::File(metadata_path + path).createDirectories();
}


void IDiskRemote::clearDirectory(const String & path)
{
    for (auto it{iterateDirectory(path)}; it->isValid(); it->next())
        if (isFile(it->path()))
            removeFile(it->path());
}


void IDiskRemote::removeDirectory(const String & path)
{
    Poco::File(metadata_path + path).remove();
}

void IDiskRemote::listFiles(const String & path, std::vector<String> & file_names)
{
    for (auto it = iterateDirectory(path); it->isValid(); it->next())
        file_names.push_back(it->name());
}

void IDiskRemote::setLastModified(const String & path, const Poco::Timestamp & timestamp)
{
    Poco::File(metadata_path + path).setLastModified(timestamp);
}

Poco::Timestamp IDiskRemote::getLastModified(const String & path)
{
    return Poco::File(metadata_path + path).getLastModified();
}

}
