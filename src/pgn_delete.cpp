#include "gambitree/pgn_delete.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <sstream>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace gambitree {
namespace {

const uint64_t kDeleteLimit = static_cast<uint64_t>(32) * 1024U * 1024U;
const size_t kCopyBufferSize = 64U * 1024U;

uint32_t rotate_right(uint32_t value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

class Sha256 {
public:
    Sha256() : bit_count_(0), used_(0)
    {
        static const uint32_t initial[8] = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
        };
        std::memcpy(state_, initial, sizeof(state_));
    }

    void update(const unsigned char* bytes, size_t length)
    {
        size_t offset = 0;
        bit_count_ += static_cast<uint64_t>(length) * 8U;
        while (offset < length) {
            size_t amount = 64U - used_;
            if (amount > length - offset)
                amount = length - offset;
            std::memcpy(block_ + used_, bytes + offset, amount);
            used_ += amount;
            offset += amount;
            if (used_ == 64U) {
                transform(block_);
                used_ = 0;
            }
        }
    }

    void finish(unsigned char digest[32])
    {
        uint64_t original_bits = bit_count_;
        unsigned char marker = 0x80U;
        unsigned char zero = 0;
        unsigned char length_bytes[8];
        int index;
        update(&marker, 1);
        while (used_ != 56U)
            update(&zero, 1);
        for (index = 7; index >= 0; --index) {
            length_bytes[index] = static_cast<unsigned char>(original_bits);
            original_bits >>= 8;
        }
        update(length_bytes, sizeof(length_bytes));
        for (index = 0; index < 8; ++index) {
            digest[index * 4] = static_cast<unsigned char>(state_[index] >> 24);
            digest[index * 4 + 1] = static_cast<unsigned char>(state_[index] >> 16);
            digest[index * 4 + 2] = static_cast<unsigned char>(state_[index] >> 8);
            digest[index * 4 + 3] = static_cast<unsigned char>(state_[index]);
        }
    }

private:
    uint32_t state_[8];
    uint64_t bit_count_;
    unsigned char block_[64];
    size_t used_;

    void transform(const unsigned char block[64])
    {
        static const uint32_t constants[64] = {
            0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,
            0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
            0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,
            0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
            0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,
            0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
            0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,
            0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
            0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,
            0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
            0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,
            0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
            0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,
            0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
            0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,
            0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
        };
        uint32_t words[64];
        uint32_t a, b, c, d, e, f, g, h;
        int i;
        for (i = 0; i < 16; ++i) {
            words[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (i = 16; i < 64; ++i) {
            uint32_t s0 = rotate_right(words[i - 15], 7) ^
                rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3);
            uint32_t s1 = rotate_right(words[i - 2], 17) ^
                rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        a = state_[0]; b = state_[1]; c = state_[2]; d = state_[3];
        e = state_[4]; f = state_[5]; g = state_[6]; h = state_[7];
        for (i = 0; i < 64; ++i) {
            uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                rotate_right(e, 25);
            uint32_t choose = (e & f) ^ (~e & g);
            uint32_t temp1 = h + sum1 + choose + constants[i] + words[i];
            uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                rotate_right(a, 22);
            uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = sum0 + majority;
            h = g; g = f; f = e; e = d + temp1;
            d = c; c = b; b = a; a = temp1 + temp2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
        state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
};

bool same_metadata(const struct stat& left, const struct stat& right)
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
        left.st_mode == right.st_mode && left.st_nlink == right.st_nlink &&
        left.st_size == right.st_size && left.st_mtime == right.st_mtime &&
        left.st_ctime == right.st_ctime &&
        left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
        left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

bool hash_fd(int fd, uint64_t expected_size, unsigned char digest[32],
             std::string& error)
{
    Sha256 hash;
    unsigned char buffer[kCopyBufferSize];
    uint64_t offset = 0;
    while (offset < expected_size) {
        size_t wanted = static_cast<size_t>(expected_size - offset);
        ssize_t got;
        if (wanted > sizeof(buffer))
            wanted = sizeof(buffer);
        do {
            got = pread(fd, buffer, wanted, static_cast<off_t>(offset));
        } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            error = got == 0 ? "file changed while fingerprinting" :
                std::string("cannot read file: ") + std::strerror(errno);
            return false;
        }
        hash.update(buffer, static_cast<size_t>(got));
        offset += static_cast<uint64_t>(got);
    }
    hash.finish(digest);
    return true;
}

bool snapshot_fd(int fd, struct stat& status, unsigned char digest[32],
                 std::string& error)
{
    struct stat after;
    if (fstat(fd, &status) != 0) {
        error = std::string("cannot inspect file: ") + std::strerror(errno);
        return false;
    }
    if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_size < 0) {
        error = "file is not a single-link regular file";
        return false;
    }
    if (static_cast<uint64_t>(status.st_size) > kDeleteLimit) {
        error = "file is larger than the 32 MiB deletion limit";
        return false;
    }
    if (!hash_fd(fd, static_cast<uint64_t>(status.st_size), digest, error))
        return false;
    if (fstat(fd, &after) != 0 || !same_metadata(status, after)) {
        error = "file changed while reading its fingerprint";
        return false;
    }
    return true;
}

int open_source(const std::string& path, std::string& error)
{
    struct stat link_status;
    int fd;
    if (lstat(path.c_str(), &link_status) != 0) {
        error = std::string("cannot inspect source file: ") + std::strerror(errno);
        return -1;
    }
    if (!S_ISREG(link_status.st_mode)) {
        error = "source is not a regular, non-symlink file";
        return -1;
    }
    if (link_status.st_nlink != 1) {
        error = "source has multiple hard links";
        return -1;
    }
    fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        error = std::string("cannot open source file: ") + std::strerror(errno);
    return fd;
}

bool fingerprint_fd_matches(int fd, const PgnDeleteBaseline& baseline,
                             std::string& error)
{
    struct stat status;
    unsigned char digest[32];
    int index;
    if (!snapshot_fd(fd, status, digest, error))
        return false;
    if (static_cast<uint64_t>(status.st_dev) != baseline.device ||
        static_cast<uint64_t>(status.st_ino) != baseline.inode ||
        static_cast<uint64_t>(status.st_size) != baseline.size ||
        static_cast<uint64_t>(status.st_mtime) != baseline.mtime_seconds ||
        static_cast<uint64_t>(status.st_ctime) != baseline.ctime_seconds ||
        static_cast<uint64_t>(status.st_mtim.tv_nsec) != baseline.mtime_nanoseconds ||
        static_cast<uint64_t>(status.st_ctim.tv_nsec) != baseline.ctime_nanoseconds ||
        static_cast<uint32_t>(status.st_mode) != baseline.mode) {
        error = "source file identity or metadata changed since it was loaded";
        return false;
    }
    for (index = 0; index < 32; ++index) {
        if (digest[index] != baseline.sha256[index]) {
            error = "source file contents changed since it was loaded";
            return false;
        }
    }
    return true;
}

bool same_path_inode(const std::string& path, const struct stat& expected,
                     std::string& error)
{
    struct stat link_status;
    struct stat path_status;
    if (lstat(path.c_str(), &link_status) != 0 ||
        !S_ISREG(link_status.st_mode) || link_status.st_nlink != 1 ||
        stat(path.c_str(), &path_status) != 0 ||
        path_status.st_dev != expected.st_dev ||
        path_status.st_ino != expected.st_ino) {
        error = "source path was replaced or removed";
        return false;
    }
    return true;
}

bool write_all(int fd, const unsigned char* bytes, size_t length,
               std::string& error)
{
    size_t offset = 0;
    while (offset < length) {
        ssize_t written;
        do {
            written = write(fd, bytes + offset, length - offset);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            error = std::string("cannot write staged PGN: ") + std::strerror(errno);
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool copy_range(int source, int destination, uint64_t begin, uint64_t end,
                std::string& error)
{
    unsigned char buffer[kCopyBufferSize];
    uint64_t offset = begin;
    if (end < begin) {
        error = "invalid indexed byte range";
        return false;
    }
    while (offset < end) {
        size_t wanted = static_cast<size_t>(end - offset);
        ssize_t got;
        if (wanted > sizeof(buffer))
            wanted = sizeof(buffer);
        do {
            got = pread(source, buffer, wanted, static_cast<off_t>(offset));
        } while (got < 0 && errno == EINTR);
        if (got <= 0) {
            error = got == 0 ? "source ended while copying indexed bytes" :
                std::string("cannot read source PGN: ") + std::strerror(errno);
            return false;
        }
        if (!write_all(destination, buffer, static_cast<size_t>(got), error))
            return false;
        offset += static_cast<uint64_t>(got);
    }
    return true;
}

bool split_path(const std::string& path, std::string& directory,
                std::string& basename)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        directory = ".";
        basename = path;
    } else {
        directory = slash == 0 ? "/" : path.substr(0, slash);
        basename = path.substr(slash + 1);
    }
    return !basename.empty();
}

class ScopedFd {
public:
    explicit ScopedFd(int descriptor) : descriptor_(descriptor) {}
    ~ScopedFd() { if (descriptor_ >= 0) close(descriptor_); }
    int get() const { return descriptor_; }
    void set(int descriptor) { descriptor_ = descriptor; }
    int close_now()
    {
        int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor >= 0 ? close(descriptor) : 0;
    }
private:
    int descriptor_;
    ScopedFd(const ScopedFd&);
    ScopedFd& operator=(const ScopedFd&);
};

class StagedFile {
public:
    explicit StagedFile(std::vector<char>& path)
        : path_(path), descriptor_(-1), published_(false), created_(false) {}
    ~StagedFile()
    {
        if (descriptor_ >= 0)
            close(descriptor_);
        if (created_ && !published_ && !path_.empty() && path_[0] != '\0')
            unlink(&path_[0]);
    }
    void set_descriptor(int descriptor)
    {
        descriptor_ = descriptor;
        created_ = descriptor >= 0;
    }
    int close_now()
    {
        int descriptor = descriptor_;
        descriptor_ = -1;
        return descriptor >= 0 ? close(descriptor) : 0;
    }
    void published() { published_ = true; }
private:
    std::vector<char>& path_;
    int descriptor_;
    bool published_;
    bool created_;
    StagedFile(const StagedFile&);
    StagedFile& operator=(const StagedFile&);
};

bool find_last_separator_line_ending(int fd, uint64_t begin,
                                     uint64_t& whitespace_begin,
                                     uint64_t& ending_begin,
                                     uint64_t& ending_end,
                                     std::string& error)
{
    unsigned char buffer[kCopyBufferSize];
    uint64_t cursor = begin;
    uint64_t last_lf = static_cast<uint64_t>(-1);
    whitespace_begin = begin;
    ending_begin = begin;
    ending_end = begin;
    while (cursor > 0) {
        uint64_t block_start = cursor > sizeof(buffer)
            ? cursor - sizeof(buffer) : 0;
        size_t amount = static_cast<size_t>(cursor - block_start);
        size_t index;
        ssize_t got;
        do {
            got = pread(fd, buffer, amount, static_cast<off_t>(block_start));
        } while (got < 0 && errno == EINTR);
        if (got != static_cast<ssize_t>(amount)) {
            error = "cannot inspect indexed game boundary";
            return false;
        }
        index = amount;
        while (index > 0) {
            unsigned char value = buffer[index - 1];
            if (value != ' ' && value != '\t' && value != '\r' &&
                value != '\n' && value != '\f' && value != '\v')
                break;
            if (value == '\n' && last_lf == static_cast<uint64_t>(-1))
                last_lf = block_start + index - 1;
            --index;
        }
        cursor = block_start + index;
        if (index != 0)
            break;
    }
    whitespace_begin = cursor;
    if (last_lf != static_cast<uint64_t>(-1)) {
        ending_begin = last_lf;
        if (last_lf > whitespace_begin) {
            unsigned char previous;
            ssize_t got;
            do {
                got = pread(fd, &previous, 1,
                            static_cast<off_t>(last_lf - 1));
            } while (got < 0 && errno == EINTR);
            if (got != 1) {
                error = "cannot inspect indexed game separator";
                return false;
            }
            if (previous == '\r')
                ending_begin = last_lf - 1;
        }
        ending_end = last_lf + 1;
    }
    return true;
}

}

PgnDeleteBaseline::PgnDeleteBaseline()
    : eligible(false), device(0), inode(0), size(0), mtime_seconds(0),
      mtime_nanoseconds(0), ctime_seconds(0), ctime_nanoseconds(0), mode(0)
{
    std::memset(sha256, 0, sizeof(sha256));
}

PgnDeleteResult::PgnDeleteResult()
    : committed(false), file_removed(false), sync_warning(false),
      has_replacement_fingerprint(false), replacement_device(0),
      replacement_inode(0), replacement_size(0),
      replacement_mtime_seconds(0), replacement_mtime_nanoseconds(0),
      replacement_mode(0)
{
    std::memset(replacement_sha256, 0, sizeof(replacement_sha256));
}

PgnDeleteBaseline capture_pgn_delete_baseline(const std::string& path)
{
    PgnDeleteBaseline baseline;
    struct stat path_status;
    struct stat status;
    unsigned char digest[32];
    std::string error;
    int fd = open_source(path, error);
    if (fd < 0) {
        baseline.reason = error;
        return baseline;
    }
    if (!snapshot_fd(fd, status, digest, error)) {
        close(fd);
        baseline.reason = error;
        return baseline;
    }
    if (lstat(path.c_str(), &path_status) != 0 ||
        !same_metadata(status, path_status)) {
        close(fd);
        baseline.reason = "source changed while it was being loaded";
        return baseline;
    }
    baseline.eligible = true;
    baseline.device = static_cast<uint64_t>(status.st_dev);
    baseline.inode = static_cast<uint64_t>(status.st_ino);
    baseline.size = static_cast<uint64_t>(status.st_size);
    baseline.mtime_seconds = static_cast<uint64_t>(status.st_mtime);
    baseline.mtime_nanoseconds = static_cast<uint64_t>(status.st_mtim.tv_nsec);
    baseline.ctime_seconds = static_cast<uint64_t>(status.st_ctime);
    baseline.ctime_nanoseconds = static_cast<uint64_t>(status.st_ctim.tv_nsec);
    baseline.mode = static_cast<uint32_t>(status.st_mode);
    std::memcpy(baseline.sha256, digest, sizeof(digest));
    close(fd);
    return baseline;
}

bool pgn_delete_baseline_matches(const std::string& path,
                                 const PgnDeleteBaseline& baseline,
                                 std::string& error)
{
    struct stat status;
    int fd;
    if (!baseline.eligible) {
        error = baseline.reason.empty()
            ? "source is not eligible for safe deletion" : baseline.reason;
        return false;
    }
    fd = open_source(path, error);
    if (fd < 0)
        return false;
    if (!fingerprint_fd_matches(fd, baseline, error)) {
        close(fd);
        return false;
    }
    if (fstat(fd, &status) != 0 || !same_path_inode(path, status, error)) {
        close(fd);
        if (error.empty())
            error = "source path changed during verification";
        return false;
    }
    close(fd);
    return true;
}

bool pgn_delete_range(const std::string& path,
                      const PgnDeleteBaseline& baseline,
                      PgnOffset begin, PgnOffset end, bool remove_file,
                      PgnDeleteResult& result)
{
    struct stat original_status;
    int source = -1;
    ScopedFd source_guard(-1);
    std::string error;
    result = PgnDeleteResult();
    try {
    if (!baseline.eligible) {
        result.message = baseline.reason.empty()
            ? "source is not eligible for safe deletion" : baseline.reason;
        return false;
    }
    source = open_source(path, error);
    if (source < 0) {
        result.message = error;
        return false;
    }
    source_guard.set(source);
    if (!fingerprint_fd_matches(source, baseline, error) ||
        fstat(source, &original_status) != 0 ||
        !same_path_inode(path, original_status, error)) {
        if (error.empty())
            error = "source changed since it was loaded";
        source_guard.close_now();
        result.message = error;
        return false;
    }
    if (remove_file) {
        std::string directory;
        std::string filename;
        int directory_fd;
        if (!pgn_delete_baseline_matches(path, baseline, error)) {
            source_guard.close_now();
            result.message = error;
            return false;
        }
        if (!split_path(path, directory, filename)) {
            source_guard.close_now();
            result.message = "invalid source path";
            return false;
        }
        if (unlink(path.c_str()) != 0) {
            result.message = std::string("cannot remove PGN file: ") +
                std::strerror(errno);
            source_guard.close_now();
            return false;
        }
        result.committed = true;
        result.file_removed = true;
        directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory_fd < 0 || fsync(directory_fd) != 0) {
            result.sync_warning = true;
            result.message = "PGN file removal succeeded, but the containing "
                "directory could not be synchronized";
        }
        if (directory_fd >= 0)
            close(directory_fd);
        source_guard.close_now();
        return true;
    }
    {
        uint64_t prefix_end = begin;
        uint64_t separator_begin = begin;
        uint64_t separator_end = begin;
        std::string directory;
        std::string filename;
        std::string pattern;
        std::vector<char> temporary_name;
        StagedFile staged_file(temporary_name);
        int destination = -1;
        int directory_fd = -1;
        struct stat staged_status;
        unsigned char bom[3];
        bool preserve_bom = false;
        uint64_t prefix_copy_begin;
        uint64_t prefix_copy_end;
        if (begin >= end || end > baseline.size ||
            !split_path(path, directory, filename)) {
            source_guard.close_now();
            result.message = error.empty() ? "invalid source path" : error;
            return false;
        }
        if (begin > 0) {
            if (!find_last_separator_line_ending(
                    source, begin, prefix_end, separator_begin,
                    separator_end, error)) {
                source_guard.close_now();
                result.message = error;
                return false;
            }
            if (end != baseline.size && prefix_end != 0) {
                prefix_end = begin;
                separator_begin = begin;
                separator_end = begin;
            }
            if (end != baseline.size && prefix_end == 0) {
                separator_begin = 0;
                separator_end = 0;
            }
        }
        if (begin == 0 && original_status.st_size >= 3) {
            ssize_t got;
            do {
                got = pread(source, bom, sizeof(bom), 0);
            } while (got < 0 && errno == EINTR);
            preserve_bom = got == 3 && bom[0] == 0xef &&
                bom[1] == 0xbb && bom[2] == 0xbf;
        }
        prefix_copy_begin = preserve_bom ? 3 : 0;
        prefix_copy_end = prefix_end < prefix_copy_begin
            ? prefix_copy_begin : prefix_end;
        pattern = directory + "/." + filename + ".gambitree-delete.XXXXXX";
        temporary_name.assign(pattern.begin(), pattern.end());
        temporary_name.push_back('\0');
        destination = mkstemp(&temporary_name[0]);
        if (destination < 0) {
            source_guard.close_now();
            result.message = std::string("cannot create staged PGN: ") +
                std::strerror(errno);
            return false;
        }
        staged_file.set_descriptor(destination);
        if (fstat(destination, &staged_status) != 0 ||
            ((staged_status.st_uid != original_status.st_uid ||
              staged_status.st_gid != original_status.st_gid) &&
             fchown(destination, original_status.st_uid,
                    original_status.st_gid) != 0) ||
            (preserve_bom &&
             !copy_range(source, destination, 0, sizeof(bom), error)) ||
            !copy_range(source, destination, prefix_copy_begin,
                        prefix_copy_end, error) ||
            !copy_range(source, destination, separator_begin, separator_end,
                        error) ||
            !copy_range(source, destination, end,
                        static_cast<uint64_t>(original_status.st_size), error) ||
            fchmod(destination, original_status.st_mode & 07777) != 0 ||
            fsync(destination) != 0 || fstat(destination, &staged_status) != 0 ||
            staged_status.st_size < 0 ||
            !hash_fd(destination, static_cast<uint64_t>(staged_status.st_size),
                     result.replacement_sha256, error)) {
            if (error.empty())
                error = std::string("cannot prepare or synchronize staged PGN: ") +
                    std::strerror(errno);
            staged_file.close_now();
            source_guard.close_now();
            result.message = error;
            return false;
        }
        if (staged_file.close_now() != 0) {
            error = std::string("cannot close staged PGN: ") +
                std::strerror(errno);
            source_guard.close_now();
            result.message = error;
            return false;
        }
        destination = -1;
        if (!fingerprint_fd_matches(source, baseline, error) ||
            !same_path_inode(path, original_status, error)) {
            source_guard.close_now();
            result.message = error.empty()
                ? "source changed before replacement" : error;
            return false;
        }
        if (rename(&temporary_name[0], path.c_str()) != 0) {
            error = std::string("cannot publish changed PGN: ") +
                std::strerror(errno);
            source_guard.close_now();
            result.message = error;
            return false;
        }
        staged_file.published();
        result.committed = true;
        result.has_replacement_fingerprint = true;
        result.replacement_device = static_cast<uint64_t>(staged_status.st_dev);
        result.replacement_inode = static_cast<uint64_t>(staged_status.st_ino);
        result.replacement_size = static_cast<uint64_t>(staged_status.st_size);
        result.replacement_mtime_seconds =
            static_cast<uint64_t>(staged_status.st_mtime);
        result.replacement_mtime_nanoseconds =
            static_cast<uint64_t>(staged_status.st_mtim.tv_nsec);
        result.replacement_mode = static_cast<uint32_t>(staged_status.st_mode);
        directory_fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory_fd < 0 || fsync(directory_fd) != 0) {
            result.sync_warning = true;
            result.message = "PGN replacement succeeded, but the containing "
                "directory could not be synchronized";
        }
        if (directory_fd >= 0)
            close(directory_fd);
        source_guard.close_now();
        return true;
    }
    } catch (const std::bad_alloc&) {
        result.message = "insufficient memory while preparing safe PGN deletion";
        return false;
    }
}

}
