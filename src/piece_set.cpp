#include "gambitree/piece_set.h"

#include "gambitree/chess.h"

namespace gambitree {

namespace {

/* Meridian targets preserve the source SVG's relative silhouette sizes. At
   S=84, they correspond to scaling the original 64px artwork canvas to 76px;
   crop padding is accounted for from each type's source alpha bounds. Offsets
   align the bases to the common baseline already present in the SVG artwork. */
const PieceSetDescriptor kPieceSets[] = {
    {
        "gambitree", "Gambitree Classic", "classic", 4, 8,
        { 0, 54, 60, 63, 55, 66, 69 },
        { 0, 0, 2, 1, 3, -1, 0 }
    },
    {
        "meridian-shaded", "Meridian Shaded", "meridian-shaded", 4, 8,
        { 0, 61, 64, 65, 60, 66, 67 },
        { 0, 2, 2, 2, 2, 1, 1 }
    },
    {
        /* 512px masters at a common 80px canvas scale for S=84, including
           8px crop padding. Queen geometry averages the two source colours;
           each raster's aspect ratio is preserved by the existing scaler. */
        "cburnett", "Cburnett", "cburnett", 4, 8,
        { 0, 60, 62, 65, 59, 66, 65 },
        { 0, 3, 1, 0, 3, 1, 1 }
    }
};

}

std::size_t piece_set_count()
{
    return sizeof(kPieceSets) / sizeof(kPieceSets[0]);
}

const PieceSetDescriptor* piece_set_at(std::size_t index)
{
    return index < piece_set_count() ? &kPieceSets[index] : 0;
}

const PieceSetDescriptor* find_piece_set(const std::string& id)
{
    std::size_t index;
    for (index = 0; index < piece_set_count(); ++index) {
        if (id == kPieceSets[index].id)
            return &kPieceSets[index];
    }
    return 0;
}

const PieceSetDescriptor& default_piece_set()
{
    return kPieceSets[0];
}

std::string canonical_piece_set_id(const std::string& id)
{
    if (id == "classic")
        return default_piece_set().id;
    const PieceSetDescriptor* descriptor = find_piece_set(id);
    return descriptor ? descriptor->id : default_piece_set().id;
}

std::string next_piece_set_id(const std::string& current_id)
{
    std::size_t index;
    for (index = 0; index < piece_set_count(); ++index) {
        if (current_id == kPieceSets[index].id ||
            (index == 0 && current_id == "classic"))
            return kPieceSets[(index + 1) % piece_set_count()].id;
    }
    return default_piece_set().id;
}

std::string initial_piece_set_id(const std::string& command_line_id,
                                 bool command_line_was_explicit,
                                 const std::string& persisted_id)
{
    if (command_line_was_explicit)
        return command_line_id == "classic" ? default_piece_set().id
                                             : command_line_id;
    if (find_piece_set(persisted_id))
        return persisted_id;
    return default_piece_set().id;
}

}
