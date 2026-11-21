#ifndef GAMBITREE_METADATA_LAYOUT_H
#define GAMBITREE_METADATA_LAYOUT_H

#include <string>
#include <vector>

#include "gambitree/notation_layout.h"

namespace gambitree {

struct MetadataLine {
    std::string text;
    bool primary;
    MetadataLine();
    MetadataLine(const std::string& value, bool is_primary);
};

class MetadataLayout {
public:
    static std::vector<MetadataLine> build(const PgnGame& game, int width,
                                           const NotationFontMetrics& metrics,
                                           bool figurines = false);
};

}

#endif
