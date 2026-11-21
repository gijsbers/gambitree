#include "gambitree/asset_locator.h"

#include <limits.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace gambitree {

namespace {

std::string parent_directory(const std::string& path)
{
    std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return "";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

bool regular_file(const std::string& path)
{
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode);
}

}

std::string AssetLocator::executable_path()
{
    char buffer[PATH_MAX + 1];
    ssize_t length = readlink("/proc/self/exe", buffer, PATH_MAX);
    if (length < 0 || length >= PATH_MAX) return "";
    buffer[length] = '\0';
    return buffer;
}

std::string AssetLocator::executable_directory()
{ return parent_directory(executable_path()); }

std::vector<std::string> AssetLocator::piece_roots(const std::string& installed_data_root,
                                                    const std::string& theme,
                                                    const std::string& executable)
{
    std::vector<std::string> roots;
    std::string directory = parent_directory(executable);
    if (!installed_data_root.empty()) roots.push_back(installed_data_root + "/pieces/" + theme);
    if (!directory.empty()) roots.push_back(directory + "/assets/pieces/" + theme);
    /* Kept only for non-Linux or unusually constrained development launches. */
    roots.push_back("assets/pieces/" + theme);
    return roots;
}

std::vector<std::string> AssetLocator::icon_path_candidates(const std::string& installed_data_root,
                                                             int size,
                                                             const std::string& executable)
{
    std::vector<std::string> paths;
    std::string directory = parent_directory(executable);
    std::ostringstream dimension;
    if (size < 1 || size > 512) return paths;
    dimension << size;
    if (!installed_data_root.empty())
        paths.push_back(installed_data_root + "/../icons/hicolor/" + dimension.str() + "x" +
                        dimension.str() + "/apps/gambitree.png");
    if (!directory.empty())
        paths.push_back(directory + "/assets/icons/gambitree-" + dimension.str() + ".png");
    paths.push_back("assets/icons/gambitree-" + dimension.str() + ".png");
    return paths;
}

std::vector<std::string> AssetLocator::board_texture_candidates(
    const std::string& installed_data_root, const std::string& filename,
    const std::string& executable)
{
    std::vector<std::string> paths;
    std::string directory;
    if (filename.empty() || filename.find('/') != std::string::npos ||
        filename == "." || filename == "..")
        return paths;
    directory = parent_directory(executable);
    if (!installed_data_root.empty())
        paths.push_back(installed_data_root + "/boards/" + filename);
    if (!directory.empty())
        paths.push_back(directory + "/assets/boards/" + filename);
    paths.push_back("assets/boards/" + filename);
    return paths;
}

bool AssetLocator::contains_piece_set(const std::string& root)
{
    static const char* const names[] = {
        "white-king", "white-queen", "white-rook", "white-bishop", "white-knight", "white-pawn",
        "black-king", "black-queen", "black-rook", "black-bishop", "black-knight", "black-pawn"
    };
    std::size_t index;
    for (index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        if (!regular_file(root + "/" + names[index] + ".png")) return false;
    }
    return true;
}

bool AssetLocator::find_piece_root(const std::string& installed_data_root, const std::string& theme,
                                   std::string& root, std::string& executable)
{
    std::vector<std::string> roots;
    std::size_t index;
    executable = executable_path();
    roots = piece_roots(installed_data_root, theme, executable);
    for (index = 0; index < roots.size(); ++index) {
        if (contains_piece_set(roots[index])) { root = roots[index]; return true; }
    }
    root.clear();
    return false;
}

}
