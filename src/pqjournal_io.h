// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQJOURNAL_IO_H
#define ORGANICLIFE_PQJOURNAL_IO_H

#include <fs.h>
#include <cstdint>

// Journal-only storage operations; callers serialize I/O and trust the journal
// directory and its ancestors (no concurrent hostile path replacement). The
// stable marker's OS lease is independent of replaced files.
namespace pqjournal { namespace io {
enum class Mode { APPEND, CREATE, REWRITE, LOCK_EXISTING, LOCK_CREATE };
bool DirectorySafe(const fs::path& directory);
int Open(const fs::path& path, Mode mode);
void Close(int fd);
FILE* Stream(int fd, const char* mode); // On success FILE owns the descriptor.
int Descriptor(FILE* file);
int64_t Size(int fd); // -1 on failure
bool WriteAll(int fd, const unsigned char* data, size_t size);
bool Sync(int fd); // Strict: unsupported flushing is failure.
bool Truncate(int fd, size_t size);
bool Replace(const fs::path& source, const fs::path& destination);
// POSIX directory fsync; Windows metadata durability comes from write-through
// file operations and FlushFileBuffers, and this checks the supported directory.
bool SyncParent(const fs::path& file);
} }
#endif
