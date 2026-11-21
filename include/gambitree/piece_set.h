#ifndef GAMBITREE_PIECE_SET_H
#define GAMBITREE_PIECE_SET_H

#include <cstddef>
#include <string>

namespace gambitree {

/* Immutable description of a bundled chess-piece artwork set. Type-indexed
   geometry uses EMPTY, pawn, knight, bishop, rook, queen, king order. */
struct PieceSetDescriptor {
    const char* id;
    const char* display_name;
    const char* asset_directory;
    unsigned char alpha_threshold;
    int crop_padding;
    int target_heights_at_84[7];
    int vertical_offsets_at_84[7];
};

std::size_t piece_set_count();
const PieceSetDescriptor* piece_set_at(std::size_t index);
const PieceSetDescriptor* find_piece_set(const std::string& id);
const PieceSetDescriptor& default_piece_set();
std::string canonical_piece_set_id(const std::string& id);
std::string next_piece_set_id(const std::string& current_id);
std::string initial_piece_set_id(const std::string& command_line_id,
                                 bool command_line_was_explicit,
                                 const std::string& persisted_id);

}

#endif
