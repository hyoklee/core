/*
 * Copyright (c) 2026, IOWarp contributors. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file ownership_ledger.h
 * @brief Durable, non-expiring ownership maintained by a fixed authority.
 *
 * This ledger belongs on private, persistent storage of the authority host. It
 * is not a lock file on the shared resource. Clients must route every request
 * to that authority; the process-exclusive handle only prevents two local
 * runtime instances from opening the same authority ledger.
 */
#ifndef CLIO_ADMIN_OWNERSHIP_LEDGER_H_
#define CLIO_ADMIN_OWNERSHIP_LEDGER_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/vfs.h>
#endif
#endif

namespace clio::run::admin {

/** Public holder information. Secret owner nonces never leave the ledger. */
struct OwnershipRecord {
  std::string authority_uuid;
  uint64_t revision = 0;
  bool held = false;
  std::string owner_label;
  std::string holder_metadata;
};

/** Outcome and current public holder information for an ownership operation. */
struct OwnershipResult {
  std::string outcome;
  OwnershipRecord record;
};

/**
 * A process-exclusive, append-only ownership authority.
 *
 * Acknowledgements follow durable writes. An I/O or recovery failure makes
 * this instance unusable until an operator resolves the cause and restarts it.
 * There are deliberately no deadlines, expiry, stealing, or force release.
 */
class OwnershipLedger {
 public:
  /** Open and recover the authority at an explicit absolute ledger path. */
  explicit OwnershipLedger(const std::string& path) {
#if !defined(__linux__)
    // Remote clients remain supported on every client platform. Authority
    // storage qualification and directory durability are Linux-only in v1.
    throw std::runtime_error("ownership authority requires qualified Linux local storage");
#endif
    const auto file = std::filesystem::path(path);
    if (!file.is_absolute() || file.filename().empty()) {
      throw std::invalid_argument("ownership ledger requires an absolute file path");
    }
    try {
      const bool created = Open(file);
      if (created) {
        authority_uuid_ = NewUuid();
        Append(EncodeHeader());
      } else {
        Recover();
      }
      SyncDirectory(file.parent_path());
    } catch (...) {
      Close();
      throw;
    }
  }

  /** Release the authority's process-exclusive file handle. */
  ~OwnershipLedger() { Close(); }

  OwnershipLedger(const OwnershipLedger&) = delete;
  OwnershipLedger& operator=(const OwnershipLedger&) = delete;

  /** Inspect a resource without revealing its owner nonce. */
  OwnershipResult Inspect(const std::string& resource) {
    std::lock_guard<std::mutex> lock(mutex_);
    CheckHealthy();
    ValidateResource(resource);
    return {"inspected", Public(resource)};
  }

  /** Acquire a free resource, or idempotently retry the current ownership. */
  OwnershipResult Acquire(const std::string& resource,
                          const std::string& owner_nonce,
                          const std::string& owner_label,
                          const std::string& expected_authority_uuid,
                          const std::string& holder_metadata) {
    std::lock_guard<std::mutex> lock(mutex_);
    CheckHealthy();
    ValidateResource(resource);
    ValidateNonce(owner_nonce);
    CheckAuthority(expected_authority_uuid);
    if (owner_label.empty() || owner_label.size() > 256 ||
        holder_metadata.size() > 8192) {
      throw std::invalid_argument("ownership holder metadata exceeds limits");
    }
    const auto current = entries_.find(resource);
    if (current != entries_.end() && current->second.held) {
      const bool retry = current->second.owner_nonce == owner_nonce;
      return {retry ? "acquired" : "busy", Public(resource)};
    }
    Entry next{NextRevision(), true, owner_nonce, owner_label, holder_metadata};
    Commit(resource, next);
    return {"acquired", Public(resource)};
  }

  /** Release only the exact currently held authority/revision/nonce token. */
  OwnershipResult Release(const std::string& resource,
                          const std::string& expected_authority_uuid,
                          uint64_t revision, const std::string& owner_nonce) {
    std::lock_guard<std::mutex> lock(mutex_);
    CheckHealthy();
    ValidateResource(resource);
    ValidateNonce(owner_nonce);
    CheckAuthority(expected_authority_uuid);
    const auto current = entries_.find(resource);
    if (current == entries_.end() || !current->second.held ||
        current->second.revision != revision ||
        current->second.owner_nonce != owner_nonce) {
      return {"rejected", Public(resource)};
    }
    Entry free{NextRevision(), false, "", "", ""};
    Commit(resource, free);
    return {"released", Public(resource)};
  }

 private:
  struct Entry {
    uint64_t revision = 0;
    bool held = false;
    std::string owner_nonce;
    std::string owner_label;
    std::string holder_metadata;
  };

  static constexpr uint32_t kMaxFrame = 16384;
  std::mutex mutex_;
  bool healthy_ = true;
  std::string authority_uuid_;
  uint64_t revision_ = 0;
  std::map<std::string, Entry> entries_;
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif

  /** Generate a persistent authority UUID; it is an identity, not a secret. */
  static std::string NewUuid() {
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    const char* hex = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < bytes.size(); ++i) {
      if (i == 4 || i == 6 || i == 8 || i == 10) result += '-';
      result += hex[bytes[i] >> 4];
      result += hex[bytes[i] & 15];
    }
    return result;
  }

  static void ValidateResource(const std::string& resource) {
    if (resource.empty() || resource.size() > 1024 ||
        resource.find('\0') != std::string::npos) {
      throw std::invalid_argument("invalid ownership resource identity");
    }
  }

  static void ValidateNonce(const std::string& nonce) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") !=
                                  std::string::npos) {
      throw std::invalid_argument("owner nonce must be 32 random bytes in lowercase hex");
    }
  }

  static void ValidateAuthorityUuid(const std::string& value) {
    if (value.size() != 36) {
      throw std::runtime_error("invalid ownership authority UUID");
    }
    for (size_t index = 0; index < value.size(); ++index) {
      const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
      const char byte = value[index];
      if ((separator && byte != '-') ||
          (!separator && !((byte >= '0' && byte <= '9') ||
                           (byte >= 'a' && byte <= 'f')))) {
        throw std::runtime_error("invalid ownership authority UUID");
      }
    }
  }

  void CheckHealthy() const {
    if (!healthy_) throw std::runtime_error("ownership authority requires recovery");
  }

  void CheckAuthority(const std::string& expected) const {
    if (expected != authority_uuid_) {
      throw std::invalid_argument("ownership authority UUID mismatch");
    }
  }

  uint64_t NextRevision() const {
    if (revision_ == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("ownership revision exhausted");
    }
    return revision_ + 1;
  }

  OwnershipRecord Public(const std::string& resource) const {
    const auto entry = entries_.find(resource);
    if (entry == entries_.end()) return {authority_uuid_, 0, false, "", ""};
    const auto& value = entry->second;
    return {authority_uuid_, value.revision, value.held, value.owner_label,
            value.holder_metadata};
  }

  void Commit(const std::string& resource, const Entry& next) {
    try {
      Append(EncodeEntry(resource, next));
      entries_[resource] = next;
      revision_ = next.revision;
    } catch (...) {
      healthy_ = false;
      throw;
    }
  }

  static void PutInteger(std::string& bytes, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      bytes += static_cast<char>((value >> (i * 8)) & 0xff);
    }
  }

  static uint64_t GetInteger(const std::string& bytes, size_t& offset,
                             size_t width) {
    if (width > bytes.size() || offset > bytes.size() - width) {
      throw std::runtime_error("truncated ownership ledger integer");
    }
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) {
      value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset++]))
               << (i * 8);
    }
    return value;
  }

  static void PutString(std::string& bytes, const std::string& value) {
    PutInteger(bytes, value.size(), 4);
    bytes += value;
  }

  static std::string GetString(const std::string& bytes, size_t& offset) {
    const auto size = GetInteger(bytes, offset, 4);
    if (size > bytes.size() - offset) {
      throw std::runtime_error("truncated ownership ledger string");
    }
    auto value = bytes.substr(offset, static_cast<size_t>(size));
    offset += static_cast<size_t>(size);
    return value;
  }

  static uint32_t Crc32(const std::string& bytes) {
    uint32_t crc = 0xffffffffU;
    for (unsigned char byte : bytes) {
      crc ^= byte;
      for (int bit = 0; bit < 8; ++bit) {
        crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
      }
    }
    return ~crc;
  }

  std::string EncodeHeader() const {
    std::string bytes = "CLIOOWN1";
    PutString(bytes, authority_uuid_);
    return bytes;
  }

  static std::string EncodeEntry(const std::string& resource, const Entry& entry) {
    std::string bytes(1, entry.held ? 'H' : 'F');
    PutInteger(bytes, entry.revision, 8);
    PutString(bytes, resource);
    PutString(bytes, entry.owner_nonce);
    PutString(bytes, entry.owner_label);
    PutString(bytes, entry.holder_metadata);
    return bytes;
  }

  void RecoverEntry(const std::string& bytes) {
    size_t offset = 1;
    Entry next;
    if (bytes.empty() || (bytes[0] != 'H' && bytes[0] != 'F')) {
      throw std::runtime_error("invalid ownership ledger record kind");
    }
    next.held = bytes[0] == 'H';
    next.revision = GetInteger(bytes, offset, 8);
    const auto resource = GetString(bytes, offset);
    next.owner_nonce = GetString(bytes, offset);
    next.owner_label = GetString(bytes, offset);
    next.holder_metadata = GetString(bytes, offset);
    ValidateResource(resource);
    if (offset != bytes.size() || next.revision != NextRevision()) {
      throw std::runtime_error("invalid ownership ledger revision or trailing data");
    }
    const auto previous = entries_.find(resource);
    const bool held = previous != entries_.end() && previous->second.held;
    if (next.held) {
      ValidateNonce(next.owner_nonce);
      if (held || next.owner_label.empty() || next.owner_label.size() > 256 ||
          next.holder_metadata.size() > 8192) {
        throw std::runtime_error("invalid ownership acquisition in ledger");
      }
    } else if (!held || !next.owner_nonce.empty() || !next.owner_label.empty() ||
               !next.holder_metadata.empty()) {
      throw std::runtime_error("invalid ownership release in ledger");
    }
    entries_[resource] = std::move(next);
    revision_ = entries_[resource].revision;
  }

  void Recover() {
    bool first = true;
    std::string prefix;
    while (ReadExact(prefix, 8, true)) {
      size_t offset = 0;
      const auto length = GetInteger(prefix, offset, 4);
      const auto crc = GetInteger(prefix, offset, 4);
      if (length == 0 || length > kMaxFrame) {
        throw std::runtime_error("invalid ownership ledger frame length");
      }
      std::string bytes;
      ReadExact(bytes, static_cast<size_t>(length), false);
      if (Crc32(bytes) != crc) {
        throw std::runtime_error("ownership ledger checksum mismatch");
      }
      if (first) {
        offset = 8;
        if (bytes.substr(0, 8) != "CLIOOWN1") {
          throw std::runtime_error("unsupported ownership ledger format");
        }
        authority_uuid_ = GetString(bytes, offset);
        ValidateAuthorityUuid(authority_uuid_);
        if (authority_uuid_.size() != 36 || offset != bytes.size()) {
          throw std::runtime_error("invalid ownership authority header");
        }
        first = false;
      } else {
        RecoverEntry(bytes);
      }
    }
    if (first) throw std::runtime_error("empty ownership ledger requires operator recovery");
  }

  void Append(const std::string& payload) {
    if (payload.empty() || payload.size() > kMaxFrame) {
      throw std::runtime_error("ownership ledger frame exceeds limit");
    }
    std::string frame;
    PutInteger(frame, payload.size(), 4);
    PutInteger(frame, Crc32(payload), 4);
    frame += payload;
    WriteAll(frame);
    SyncFile();
  }

  bool Open(const std::filesystem::path& file) {
#ifdef _WIN32
    handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ, nullptr, CREATE_NEW,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    bool created = handle_ != INVALID_HANDLE_VALUE;
    if (!created && GetLastError() == ERROR_FILE_EXISTS) {
      handle_ = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    }
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("cannot exclusively open ownership authority ledger");
    }
    return created;
#else
    fd_ = open(file.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    const bool created = fd_ >= 0;
    if (!created && errno == EEXIST) {
      fd_ = open(file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      throw std::runtime_error("cannot exclusively open ownership authority ledger");
    }
    struct stat status {};
    if (fstat(fd_, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
      throw std::runtime_error("ownership ledger must be a private, owned regular file");
    }
    CheckLocalFilesystem();
    return created;
#endif
  }

  void CheckLocalFilesystem() const {
#ifdef __linux__
    struct statfs status {};
    if (fstatfs(fd_, &status) != 0) {
      throw std::runtime_error("cannot inspect ownership authority filesystem");
    }
    // NFS, SMB/CIFS, SMB2, and volatile tmpfs/ramfs cannot host this authority.
    const auto type = static_cast<unsigned long>(status.f_type);
    if (type == 0x6969UL || type == 0xff534d42UL || type == 0xfe534d42UL ||
        type == 0x01021994UL || type == 0x858458f6UL) {
      throw std::runtime_error("ownership authority requires private persistent local storage");
    }
#endif
  }

  bool ReadExact(std::string& bytes, size_t size, bool allow_eof) {
    bytes.assign(size, '\0');
    size_t offset = 0;
    while (offset < size) {
#ifdef _WIN32
      DWORD count = 0;
      if (!ReadFile(handle_, bytes.data() + offset,
                    static_cast<DWORD>(size - offset), &count, nullptr)) {
        throw std::runtime_error("cannot read ownership authority ledger");
      }
#else
      const auto count = read(fd_, bytes.data() + offset, size - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) throw std::runtime_error("cannot read ownership authority ledger");
#endif
      if (count == 0) {
        if (allow_eof && offset == 0) return false;
        throw std::runtime_error("truncated ownership ledger requires operator recovery");
      }
      offset += static_cast<size_t>(count);
    }
    return true;
  }

  void WriteAll(const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
#ifdef _WIN32
      DWORD count = 0;
      if (!WriteFile(handle_, bytes.data() + offset,
                     static_cast<DWORD>(bytes.size() - offset), &count, nullptr)) {
        throw std::runtime_error("cannot append ownership authority ledger");
      }
#else
      const auto count = write(fd_, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) throw std::runtime_error("cannot append ownership authority ledger");
#endif
      if (count == 0) throw std::runtime_error("short ownership ledger write");
      offset += static_cast<size_t>(count);
    }
  }

  void SyncFile() {
#ifdef _WIN32
    if (!FlushFileBuffers(handle_)) {
#else
    if (fsync(fd_) != 0) {
#endif
      throw std::runtime_error("ownership authority durability barrier failed");
    }
  }

  static void SyncDirectory(const std::filesystem::path& directory) {
#ifdef _WIN32
    // CREATE_NEW + FILE_FLAG_WRITE_THROUGH and FlushFileBuffers persist the
    // file. Windows does not expose POSIX directory fsync through this API.
    HANDLE parent = CreateFileW(directory.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parent == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("cannot verify ownership ledger parent directory");
    }
    CloseHandle(parent);
#else
    const int parent = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) throw std::runtime_error("cannot open ownership ledger parent directory");
    const int result = fsync(parent);
    const int closed = close(parent);
    if (result != 0 || closed != 0) {
      throw std::runtime_error("ownership ledger directory durability barrier failed");
    }
#endif
  }

  void Close() noexcept {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
#else
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
#endif
  }
};

}  // namespace clio::run::admin

#endif  // CLIO_ADMIN_OWNERSHIP_LEDGER_H_
