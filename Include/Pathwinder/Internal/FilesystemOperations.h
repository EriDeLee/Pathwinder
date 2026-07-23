/***************************************************************************************************
 * Pathwinder
 *   Path redirection for files, directories, and registry entries.
 ***************************************************************************************************
 * Authored by Samuel Grossman
 * Copyright (c) 2022-2025
 ***********************************************************************************************//**
 * @file FilesystemOperations.h
 *   Declaration of functions that provide an abstraction for filesystem operations executed
 *   internally.
 **************************************************************************************************/

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

#include <Infra/Core/Strings.h>
#include <Infra/Core/TemporaryBuffer.h>
#include <Infra/Core/ValueOrError.h>

#include "ApiWindows.h"

namespace Pathwinder
{
  namespace FilesystemOperations
  {
    /// Closes a handle that was previously opened by calling filesystem operation functions.
    /// @param [in] handle Handle to be closed.
    /// @return Result of the underlying system call that closes the handle.
    NTSTATUS CloseHandle(HANDLE handle);

    /// Copies the contents and basic metadata (attributes and timestamps) of a single existing
    /// file to a destination path. Both paths are used verbatim, without any filesystem
    /// redirection, so callers must supply fully-resolved absolute paths. All internal system
    /// calls bypass Pathwinder's own hooks, so this function is safe to call from within
    /// redirection logic without causing re-entrancy. Any missing ancestor directories of the
    /// destination are created. An existing destination file is overwritten. This is the
    /// primitive used to implement copy-up: materializing an origin-side (for example, C:) file
    /// on the target side (for example, D:) before a write is permitted to proceed, so that the
    /// origin side is never modified.
    /// @param [in] absoluteSourcePath Absolute path of the existing file to copy from.
    /// @param [in] absoluteDestinationPath Absolute path of the file to create or overwrite.
    /// @return System call return code indicating the result of the operation.
    NTSTATUS CopySingleFile(
        std::wstring_view absoluteSourcePath, std::wstring_view absoluteDestinationPath);

    /// Filename suffix that identifies a whiteout (tombstone) marker. A zero-length file whose
    /// name is a real filename with this suffix appended records that the same-named file, as it
    /// would otherwise be resolved on the origin side (for example, C:), is logically deleted in
    /// the overlay. The marker is created on the target side (for example, D:) next to where the
    /// effective file would live. Its semantics are latent: while a real target-side file with
    /// the base name exists, the marker is inert (the file is live); the instant that target-side
    /// file is deleted (by any means), the marker takes effect and hides the origin-side file.
    inline constexpr std::wstring_view kWhiteoutFilenameSuffix = L".__pw_wh__";

    /// Determines whether the specified filename (a single path component, not a full path) is a
    /// whiteout marker filename, that is, whether it ends with the whiteout filename suffix.
    /// @param [in] filename Single-component filename to test.
    /// @return `true` if the filename is a whiteout marker filename, `false` otherwise.
    inline bool IsWhiteoutFilename(std::wstring_view filename)
    {
      return filename.ends_with(kWhiteoutFilenameSuffix);
    }

    /// Creates a zero-length file at the specified absolute path, creating any missing ancestor
    /// directories, and overwriting any existing file. Used to place whiteout markers. Bypasses
    /// Pathwinder's own hooks, so it is safe to call from within redirection logic.
    /// @param [in] absolutePath Absolute path of the zero-length file to create.
    /// @return System call return code indicating the result of the operation.
    NTSTATUS CreateEmptyFile(std::wstring_view absolutePath);

    /// Scans a directory for active tombstones. A tombstone is active when a whiteout marker file
    /// (a file whose name ends with the whiteout suffix) is present but the corresponding base
    /// file (the marker filename with the suffix removed) is not, meaning the base name is
    /// logically deleted in the overlay. Bypasses Pathwinder's own hooks, so it is safe to call
    /// from within redirection logic. Returns the base filenames (single path components) that
    /// are actively tombstoned. If the directory does not exist or contains no markers, the
    /// returned container is empty.
    /// @param [in] absoluteDirectoryPath Absolute path of the directory to scan.
    /// @return Set of actively-tombstoned base filenames found in the directory.
    std::set<std::wstring, Infra::Strings::CaseInsensitiveLessThanComparator<wchar_t>>
        FindActiveTombstones(std::wstring_view absoluteDirectoryPath);

    /// Attempts to create the specified directory if it does not already exist.
    /// If needed, also attempts to create all directories that are ancestors of the specified
    /// directory.
    /// @param [in] absoluteDirectoryPath Absolute path of the directory to be created along
    /// with its hierarchy of ancestors.
    /// @return System call return code for the last system call that completed successfully.
    NTSTATUS CreateDirectoryHierarchy(std::wstring_view absoluteDirectoryPath);

    /// Attempts to delete the specified file or directory.
    /// @param [in] absolutePath Absolute path of the entity to delete.
    /// @return System call return code for the deletion operation.
    NTSTATUS Delete(std::wstring_view absolutePath);

    /// Checks if the specified filesystem entity (file, directory, or otherwise) exists.
    /// @param [in] absolutePath Absolute path of the entity to check.
    /// @return `true` if the entity exists, `false` otherwise.
    bool Exists(std::wstring_view absolutePath);

    /// Checks if the specified path exists in the filesystem as a directory.
    /// @param [in] absolutePath Absolute path of the entity to check.
    /// @return `true` if the path exists as a directory, `false` otherwise.
    bool IsDirectory(std::wstring_view absolutePath);

    /// Opens the specified directory for synchronous enumeration.
    /// @param [in] absoluteDirectoryPath Absolute path to the directory to be opened.
    /// @return Handle for the directory file on success, Windows error code on failure.
    Infra::ValueOrError<HANDLE, NTSTATUS> OpenDirectoryForEnumeration(
        std::wstring_view absoluteDirectoryPath);

    /// Attempts to enumerate the contents of the directory identified by open handle, up to
    /// whatever portion of the overall contents will fit in the specified buffer. Can be
    /// invoked multiple times on the same handle until all of the directory contents have been
    /// enumerated.
    /// @param [in] directoryHandle Open handle for the directory to enumerate.
    /// @param [in] fileInformationClass Type of information to request for each file in the
    /// directory.
    /// @param [out] enumerationBuffer Buffer into which to write the information received about
    /// the file.
    /// @param [in] enumerationBufferCapacityBytes Size of the destination buffer, in bytes.
    /// @param [in] queryFlags Optional flags for `NtQueryDirectoryFileEx` that describe any
    /// customizations to be applied to the query.
    /// @param [in] filePattern Optional file name or pattern to use for filtering the filenames
    /// that are returned in the enumeration.
    /// @return Windows error code identifying the result of the operation.
    NTSTATUS PartialEnumerateDirectoryContents(
        HANDLE directoryHandle,
        FILE_INFORMATION_CLASS fileInformationClass,
        void* enumerationBuffer,
        unsigned int enumerationBufferCapacityBytes,
        ULONG queryFlags = 0,
        std::wstring_view filePattern = std::wstring_view());

    /// Obtains the full absolute path for the specified file handle, without a Windows namespace
    /// prefix.
    /// @param [in] fileHandle File handle for which the full absolute path is desired.
    /// @return Absolute path for the file handle, or a Windows error code on failure.
    Infra::ValueOrError<Infra::TemporaryString, NTSTATUS> QueryAbsolutePathByHandle(
        HANDLE fileHandle);

    /// Obtains mode information for the specified file handle. The file handle must be open
    /// already. The mode is itself a bitmask that identifies the effective options that determine
    /// how the I/O system behaves with respect to the file.
    /// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/ns-ntifs-_file_mode_information
    /// @param [in] fileHandle File handle for which mode information is desired.
    /// @return Mode information for the file handle, or a Windows error code on failure.
    Infra::ValueOrError<ULONG, NTSTATUS> QueryFileHandleMode(HANDLE fileHandle);

    /// Obtains information about the specified file by asking the system to enumerate it via
    /// directory enumeration.
    /// @param [in] absoluteDirectoryPath Absolute path to the directory containing the file to
    /// be enumerated. Windows namespace prefix is not required.
    /// @param [in] fileName Name of the file within the directory. Must not contain any
    /// wildcards or backslashes.
    /// @param [in] fileInformationClass Type of information to obtain about the specified file.
    /// @param [out] enumerationBuffer Buffer into which to write the information received about
    /// the file.
    /// @param [in] enumerationBufferCapacityBytes Size of the destination buffer, in bytes.
    /// @return Windows error code identifying the result of the operation.
    NTSTATUS QuerySingleFileDirectoryInformation(
        std::wstring_view absoluteDirectoryPath,
        std::wstring_view fileName,
        FILE_INFORMATION_CLASS fileInformationClass,
        void* enumerationBuffer,
        unsigned int enumerationBufferCapacityBytes);
  } // namespace FilesystemOperations
} // namespace Pathwinder
