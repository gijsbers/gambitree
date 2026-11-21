#ifndef GAMBITREE_TEST_PATHS_H
#define GAMBITREE_TEST_PATHS_H

#include <cstdlib>
#include <limits.h>
#include <string>
#include <vector>
#include <unistd.h>

namespace test_paths {

inline std::string working_directory()
{
    char directory[PATH_MAX + 1];
    return getcwd(directory, sizeof(directory)) ? directory : "";
}

/* Relative TMPDIR values stay stable when a test temporarily changes cwd. */
inline std::string scratch_root(const char* temporary_directory)
{
    const std::string root = temporary_directory && *temporary_directory
        ? temporary_directory : "build";
    if (root[0] == '/') return root;
    const std::string directory = working_directory();
    return directory.empty() ? "" : directory + "/" + root;
}

inline std::string make_directory(const std::string& name,
                                  const std::string& parent = "")
{
    const std::string root = parent.empty() ? scratch_root(std::getenv("TMPDIR")) : parent;
    if (root.empty()) return "";
    const std::string pattern = root + "/" + name + ".XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const char* directory = mkdtemp(&writable[0]);
    return directory ? directory : "";
}

}

#endif
