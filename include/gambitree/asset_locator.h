#ifndef GAMBITREE_ASSET_LOCATOR_H
#define GAMBITREE_ASSET_LOCATOR_H

#include <string>
#include <vector>

namespace gambitree {

/* Locates the immutable piece-set directory without depending on the cwd. */
class AssetLocator {
public:
    static std::string executable_path();
    static std::string executable_directory();
    static std::vector<std::string> piece_roots(const std::string& installed_data_root,
                                                const std::string& theme,
                                                const std::string& executable_path);
    static std::vector<std::string> icon_path_candidates(const std::string& installed_data_root,
                                                         int size,
                                                         const std::string& executable_path);
    static std::vector<std::string> board_texture_candidates(
        const std::string& installed_data_root, const std::string& filename,
        const std::string& executable_path);
    static bool contains_piece_set(const std::string& root);
    static bool find_piece_root(const std::string& installed_data_root, const std::string& theme,
                                std::string& root, std::string& executable);
};

}

#endif
