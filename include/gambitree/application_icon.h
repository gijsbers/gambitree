#ifndef GAMBITREE_APPLICATION_ICON_H
#define GAMBITREE_APPLICATION_ICON_H

#include <string>
#include <vector>

#include "gambitree/png_image.h"

namespace gambitree {

/* Converts one RGBA image to EWMH ARGB pixels stored in Xlib's format-32
 * client representation (unsigned long, including on LP64 systems). */
bool rgba_to_ewmh_argb(const PngImage& image, std::vector<unsigned long>& pixels,
                       std::string& error);

/* Appends one width, height, pixel... record to an _NET_WM_ICON property. */
bool append_ewmh_icon_image(const PngImage& image, std::vector<unsigned long>& property,
                            std::string& error);

/* Each inner vector is one size's ordered lookup candidates. Missing or bad
 * sizes are skipped; false is returned only when no usable image remains. */
bool load_ewmh_icon_candidates(const std::vector<std::vector<std::string> >& candidates,
                               const std::vector<int>& sizes,
                               std::vector<unsigned long>& property,
                               std::string& error);

std::vector<int> application_icon_sizes();
bool load_application_icon(const std::string& installed_data_root,
                           const std::string& executable_path,
                           std::vector<unsigned long>& property,
                           std::string& error);

}

#endif
