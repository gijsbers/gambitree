#ifndef GAMBITREE_PGN_DELETE_H
#define GAMBITREE_PGN_DELETE_H

#include <stdint.h>
#include <string>

#include "gambitree/pgn_index.h"

namespace gambitree {

struct PgnDeleteBaseline {
    bool eligible;
    uint64_t device;
    uint64_t inode;
    uint64_t size;
    uint64_t mtime_seconds;
    uint64_t mtime_nanoseconds;
    uint64_t ctime_seconds;
    uint64_t ctime_nanoseconds;
    uint32_t mode;
    unsigned char sha256[32];
    std::string reason;
    PgnDeleteBaseline();
};

struct PgnDeleteResult {
    bool committed;
    bool file_removed;
    bool sync_warning;
    bool has_replacement_fingerprint;
    uint64_t replacement_device;
    uint64_t replacement_inode;
    uint64_t replacement_size;
    uint64_t replacement_mtime_seconds;
    uint64_t replacement_mtime_nanoseconds;
    uint32_t replacement_mode;
    unsigned char replacement_sha256[32];
    std::string message;
    PgnDeleteResult();
};

/* Captured when the PGN is loaded. Files above the deletion limit are not
   fingerprinted, so opening large databases does not add a full-file pass. */
PgnDeleteBaseline capture_pgn_delete_baseline(const std::string& path);
bool pgn_delete_baseline_matches(const std::string& path,
                                const PgnDeleteBaseline& baseline,
                                std::string& error);
bool pgn_delete_range(const std::string& path,
                      const PgnDeleteBaseline& baseline,
                      PgnOffset begin, PgnOffset end, bool remove_file,
                      PgnDeleteResult& result);

}

#endif
