// SPDX-License-Identifier: BSD-3-Clause
#include "storage/ring_file.h"
#include "core/endian.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace budyk {

namespace {

// File header layout (64 bytes total, spec §3.5):
//   +0   u8[8]  magic  "BDYKRB\0\x01"
//   +8   u32    version
//   +12  u8     tier (1|2|3; 0x80 for custom-level rings)
//   +13  u8[3]  pad
//   +16  u32    record_size
//   +20  u64    capacity
//   +28  u64    write_idx   records written; not 8-byte aligned, so it is
//                           copied in and out (widx_ is the atomic)
//   +36  u64    step_back   step_back_index(), 0 if none (back_ the atomic);
//                           an older budyk left it 0 and does not write it
//   +44  u8[20] reserved
constexpr size_t   kHeaderSize      = 64;
constexpr uint32_t kFormatVersion   = 1;
constexpr char     kMagicBytes[8]   = {'B', 'D', 'Y', 'K', 'R', 'B', '\0', '\x01'};
constexpr size_t   kWriteIdxOffset  = 28;
constexpr size_t   kStepBackOffset  = 36;

// Whether the header of the ring file open at `fd` is all zero bytes.
bool header_is_blank(int fd) {
    uint8_t h[kHeaderSize];
    if (::pread(fd, h, sizeof(h), 0) != static_cast<ssize_t>(sizeof(h))) return false;
    for (const uint8_t b : h) {
        if (b != 0) return false;
    }
    return true;
}

} // namespace

int RingFile::open(const char* path, uint8_t tier, uint32_t record_size, uint64_t capacity) {
    return open_impl(path, tier, record_size, capacity, false);
}

int RingFile::open_read_only(const char* path, uint8_t tier, uint32_t record_size,
                             uint64_t capacity) {
    return open_impl(path, tier, record_size, capacity, true);
}

int RingFile::open_impl(const char* path, uint8_t tier, uint32_t record_size,
                        uint64_t capacity, bool read_only) {
    last_errno_ = 0;
    if (fd_ >= 0)                    return -1;  // already open
    if (record_size == 0 || capacity == 0) return -2;

    // In 64 bits: a size_t of 32 bits (i386, armv7) cut a ring over 4 GiB
    // and the next start refused the file it had grown into.
    if (capacity > (UINT64_MAX - kHeaderSize) / record_size) return -6;
    const uint64_t file_bytes = kHeaderSize + static_cast<uint64_t>(record_size) * capacity;
    if (file_bytes > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return -6;

    // CLOEXEC: a child forked by rule exec() or an alert channel must
    // not hold the ring open — it could outlive the daemon and write
    // into a ring the next instance has already reopened.
    // 0600: the history (load, memory, process counts, file-watch events)
    // is the daemon's, like sessions.tsv; readers go through the API.
    // An existing file keeps its mode.
    // No symbolic link (O_NOFOLLOW), and below a regular file of one name:
    // root running serve or fsck --repair on the service's data_dir opened
    // whatever budyk's user had put there under a ring's name (a link to
    // a root-owned file, sized and given a header), as the lock already
    // refuses. O_NONBLOCK: a FIFO there held the open (fsck, suggest-rules).
    int fd = read_only ? ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)
                       : ::open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) {
        last_errno_ = errno;
        struct stat lst{};
        if (::lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) { last_errno_ = 0; return -17; }
        return -3;
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0)       { last_errno_ = errno; ::close(fd); return -4; }
    if (!S_ISREG(st.st_mode))        { ::close(fd); return -18; }
    if (st.st_nlink > 1)             { ::close(fd); return -19; }
    bool fresh = (st.st_size == 0);
    if (fresh && read_only)          { ::close(fd); return -6; }

    if (fresh) {
        if (::ftruncate(fd, static_cast<off_t>(file_bytes)) != 0) {
            last_errno_ = errno; ::close(fd); return -5;
        }
    } else if (static_cast<uint64_t>(st.st_size) != file_bytes) {
        ::close(fd); return -6;
    } else if (!read_only && header_is_blank(fd)) {
        // Sized but never given its header: a start that died between
        // the ftruncate and the header (a full disk, before the header
        // went through pwrite; a SIGKILL). It holds nothing: made anew,
        // where it used to stop every start with "bad magic".
        fresh = true;
    }

    if (fresh) {
        // The header goes in with pwrite before the page is mapped: the
        // file is sparse, and a store through the mapping into a hole on
        // a full filesystem is SIGBUS, not an error. Undone if it fails,
        // so the next start finds an empty file, not a blank header.
        uint8_t h[kHeaderSize] = {};
        std::memcpy(h, kMagicBytes, 8);
        le_put_u32(h + 8,  kFormatVersion);
        h[12] = tier;
        le_put_u32(h + 16, record_size);
        le_put_u64(h + 20, capacity);
        le_put_u64(h + kWriteIdxOffset, 0);
        const ssize_t w = ::pwrite(fd, h, kHeaderSize, 0);
        if (w != static_cast<ssize_t>(kHeaderSize)) {
            last_errno_ = w < 0 ? errno : ENOSPC;
            if (::ftruncate(fd, 0) != 0) { /* the blank header is made anew next time */ }
            ::close(fd);
            return -14;
        }
    }

    void* m = ::mmap(nullptr, kHeaderSize, read_only ? PROT_READ : PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)             { last_errno_ = errno; ::close(fd); return -7; }

    auto* h = static_cast<uint8_t*>(m);
    if (!fresh) {
        if (std::memcmp(h, kMagicBytes, 8) != 0) { ::munmap(m, kHeaderSize); ::close(fd); return -8; }
        if (le_get_u32(h + 8)   != kFormatVersion)  { ::munmap(m, kHeaderSize); ::close(fd); return -9; }
        if (h[12]            != tier)            { ::munmap(m, kHeaderSize); ::close(fd); return -10; }
        if (le_get_u32(h + 16)  != record_size)     { ::munmap(m, kHeaderSize); ::close(fd); return -11; }
        if (le_get_u64(h + 20)  != capacity)        { ::munmap(m, kHeaderSize); ::close(fd); return -12; }
    }

    fd_          = fd;
    read_only_   = read_only;
    tier_        = tier;
    mmap_base_   = m;
    mmap_len_    = kHeaderSize;
    record_size_ = record_size;
    capacity_    = capacity;
    widx_.store(le_get_u64(h + kWriteIdxOffset), std::memory_order_release);
    begun_.store(widx_.load(std::memory_order_relaxed), std::memory_order_release);
    back_.store(le_get_u64(h + kStepBackOffset), std::memory_order_release);
    last_ts_   = 0;
    have_last_ = false;
    if (!read_only) {
        // The newest records: the one the next append follows, and a step
        // back among them that the header does not have (an older budyk
        // wrote the ring, or a power loss left a previous lap under the
        // index).
        const uint64_t widx  = widx_.load(std::memory_order_relaxed);
        const uint64_t valid = widx < capacity_ ? widx : capacity_;
        const uint64_t look  = valid < kStepBackScan ? valid : kStepBackScan;
        scan_order(widx - look, widx);
    }
    return 0;
}

bool RingFile::raw_ts(uint64_t logical, uint64_t* ts) const {
    uint8_t b[8];
    const off_t off = static_cast<off_t>(kHeaderSize + (logical % capacity_) * record_size_);
    if (record_size_ < sizeof(b) || ::pread(fd_, b, sizeof(b), off) != static_cast<ssize_t>(sizeof(b))) {
        return false;
    }
    *ts = le_get_u64(b);
    return true;
}

void RingFile::scan_order(uint64_t from, uint64_t to) {
    const uint64_t widx   = widx_.load(std::memory_order_relaxed);
    const uint64_t oldest = widx - (widx < capacity_ ? widx : capacity_);
    if (from < oldest) from = oldest;
    // From the record before `from`, which the first of them follows (the
    // records advance_write_index() counted in follow the newest before).
    uint64_t prev = 0;
    bool     have = false;
    uint64_t newest_back = 0;
    for (uint64_t k = from > oldest ? from - 1 : from; k < to; ++k) {
        uint64_t ts = 0;
        if (!raw_ts(k, &ts)) { have = false; continue; }
        if (have && ts < prev) newest_back = k;
        prev = ts;
        have = true;
    }
    if (newest_back > back_.load(std::memory_order_relaxed)) note_step_back(newest_back);
    have_last_ = have;
    last_ts_   = have ? prev : 0;
}

void RingFile::note_step_back(uint64_t logical) {
    if (mmap_base_ != nullptr && !read_only_) {
        le_put_u64(static_cast<uint8_t*>(mmap_base_) + kStepBackOffset, logical);
    }
    back_.store(logical, std::memory_order_release);
}

int RingFile::read_header(const char* path, Header* out, int* err) {
    if (err != nullptr) *err = 0;
    if (path == nullptr || out == nullptr) return -2;
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        const int e = errno;
        struct stat lst{};
        if (::lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) return -17;
        if (err != nullptr) *err = e;
        return -3;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        if (err != nullptr) *err = errno;
        ::close(fd);
        return -4;
    }
    if (!S_ISREG(st.st_mode)) { ::close(fd); return -18; }   // a FIFO held fsck; a directory
    if (st.st_nlink > 1)      { ::close(fd); return -19; }
    if (st.st_size == 0) { ::close(fd); return -15; }   // what a failed header write leaves
    uint8_t h[kHeaderSize];
    const ssize_t n = st.st_size >= static_cast<off_t>(kHeaderSize)
                    ? ::pread(fd, h, kHeaderSize, 0) : 0;
    ::close(fd);
    if (n != static_cast<ssize_t>(kHeaderSize))       return -13;
    if (std::all_of(h, h + kHeaderSize, [](uint8_t b) { return b == 0; })) return -16;
    if (std::memcmp(h, kMagicBytes, 8) != 0)          return -8;
    if (le_get_u32(h + 8) != kFormatVersion)          return -9;
    out->tier        = h[12];
    out->record_size = le_get_u32(h + 16);
    out->capacity    = le_get_u64(h + 20);
    out->write_idx   = le_get_u64(h + kWriteIdxOffset);
    if (out->record_size == 0 || out->capacity == 0 ||
        out->capacity > (static_cast<uint64_t>(st.st_size) - kHeaderSize) / out->record_size ||
        static_cast<uint64_t>(st.st_size) !=
            kHeaderSize + static_cast<uint64_t>(out->record_size) * out->capacity) {
        return -6;
    }
    return 0;
}

void RingFile::close() {
    if (mmap_base_ != nullptr) {
        ::munmap(mmap_base_, mmap_len_);
        mmap_base_ = nullptr;
        mmap_len_  = 0;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    record_size_ = 0;
    capacity_    = 0;
    read_only_   = false;
    tier_        = 0;
    widx_.store(0, std::memory_order_release);
    begun_.store(0, std::memory_order_release);
    back_.store(0, std::memory_order_release);
    last_ts_   = 0;
    have_last_ = false;
}

int RingFile::append(const void* record, size_t len) {
    if (fd_ < 0 || read_only_ || record == nullptr) return -1;
    if (len != record_size_)                   return -2;

    const uint64_t idx  = widx_.load(std::memory_order_acquire);
    const uint64_t slot = idx % capacity_;
    const off_t    off  = static_cast<off_t>(kHeaderSize + slot * record_size_);

    // Said before the slot is touched: a reader that finds the record it
    // read may be this one (write_begun()) drops it.
    begun_.store(idx + 1, std::memory_order_release);
    ssize_t w = ::pwrite(fd_, record, record_size_, off);
    if (w != static_cast<ssize_t>(record_size_)) {
        last_errno_ = w < 0 ? errno : ENOSPC;   // a short write: out of room
        begun_.store(idx, std::memory_order_release);
        return -3;
    }

    // Stamped before the record ahead of it (a clock step back): said
    // before the index takes the record in, so a reader that sees the
    // record live sees the step too.
    const uint64_t ts = le_get_u64(static_cast<const uint8_t*>(record));
    if (have_last_ && ts < last_ts_) note_step_back(idx);
    last_ts_   = ts;
    have_last_ = true;

    // Advance write_idx only after the record is durably placed. A crash
    // between pwrite and this increment loses at most the last record.
    le_put_u64(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset, idx + 1);
    widx_.store(idx + 1, std::memory_order_release);
    return 0;
}

int RingFile::read_at(uint64_t index, void* out, size_t len) const {
    if (fd_ < 0 || out == nullptr)             return -1;
    if (len != record_size_)                   return -2;
    if (index >= capacity_)                    return -3;

    off_t off = static_cast<off_t>(kHeaderSize + index * record_size_);
    reads_.fetch_add(1, std::memory_order_relaxed);
    ssize_t r = ::pread(fd_, out, record_size_, off);
    if (r != static_cast<ssize_t>(record_size_)) return -4;
    return 0;
}

uint64_t RingFile::reads() const { return reads_.load(std::memory_order_relaxed); }
uint32_t RingFile::record_size() const { return record_size_; }

int RingFile::advance_write_index(uint64_t n) {
    if (fd_ < 0 || read_only_) return -1;
    const uint64_t from = widx_.load(std::memory_order_acquire);
    const uint64_t idx  = from + n;
    le_put_u64(static_cast<uint8_t*>(mmap_base_) + kWriteIdxOffset, idx);
    begun_.store(idx, std::memory_order_release);
    widx_.store(idx, std::memory_order_release);
    scan_order(from, idx);   // the records counted in: the next append follows the newest
    return 0;
}

uint64_t RingFile::write_index() const {
    if (fd_ < 0) return 0;
    // Read-only (fsck, suggest-rules): the header as it is now, since a
    // running daemon may be writing the ring.
    if (read_only_) {
        return le_get_u64(static_cast<const uint8_t*>(mmap_base_) + kWriteIdxOffset);
    }
    return widx_.load(std::memory_order_acquire);
}

uint64_t RingFile::write_begun() const {
    if (fd_ < 0) return 0;
    if (read_only_) return write_index();   // another process's writes: not seen
    return begun_.load(std::memory_order_acquire);
}

uint64_t RingFile::step_back_index() const {
    if (fd_ < 0) return 0;
    if (read_only_) {   // another process may be writing: the header as it is now
        return le_get_u64(static_cast<const uint8_t*>(mmap_base_) + kStepBackOffset);
    }
    return back_.load(std::memory_order_acquire);
}

uint64_t RingFile::count() const {
    const uint64_t idx = write_index();
    return idx < capacity_ ? idx : capacity_;
}

uint64_t RingFile::capacity() const { return capacity_; }

uint8_t RingFile::tier() const { return tier_; }

int RingFile::last_errno() const { return last_errno_; }

const char* RingFile::describe(int rc) {
    switch (rc) {
        case 0:   return "ok";
        case -1:  return "already open";
        case -2:  return "record size or capacity is zero";
        case -3:  return "open failed";
        case -4:  return "fstat failed";
        case -5:  return "ftruncate failed";
        case -6:  return "file size does not match record size x capacity";
        case -7:  return "mmap failed";
        case -8:  return "not a budyk ring file (bad magic)";
        case -9:  return "ring format version mismatch";
        case -10: return "ring was written for another tier";
        case -11: return "record size in the header differs";
        case -12: return "capacity in the header differs";
        case -13: return "file is shorter than a ring header";
        case -14: return "writing the header failed";
        case -15: return "empty file";
        case -16: return "blank header (all zero bytes)";
        case -17: return "a symbolic link (refused: put data_dir itself where the rings should be)";
        case -18: return "not a regular file";
        case -19: return "has another name (a hard link: refused)";
        default:  return "unknown error";
    }
}

} // namespace budyk
