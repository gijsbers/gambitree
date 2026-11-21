#ifndef GAMBITREE_PNG_IMAGE_H
#define GAMBITREE_PNG_IMAGE_H

#include <string>
#include <cstddef>
#include <vector>

namespace gambitree {

struct PngImage {
    int width;
    int height;
    std::size_t stride;
    std::vector<unsigned char> rgba;
    PngImage();
    bool valid() const;
};

struct PngAlphaBounds {
    int x;
    int y;
    int width;
    int height;
    PngAlphaBounds();
    bool valid() const;
};

struct PngResizeProfile {
    double horizontal_ms;
    double vertical_ms;
    PngResizeProfile();
};

class PngLanczos3Job {
public:
    PngLanczos3Job();
    ~PngLanczos3Job();
    bool start(const PngImage& source, int width, int height);
    bool step(std::size_t maximum_rows);
    bool ready() const;
    const PngImage& result() const;
    PngResizeProfile profile() const;
    void take_result(PngImage& result);
    void cancel();
private:
    struct Impl;
    Impl* impl_;
    PngLanczos3Job(const PngLanczos3Job&);
    PngLanczos3Job& operator=(const PngLanczos3Job&);
};

bool load_png_rgba(const std::string& path, PngImage& image, std::string& error);
bool png_alpha_bounds(const PngImage& image, PngAlphaBounds& bounds);
bool png_alpha_bounds_at_least(const PngImage& image, unsigned char threshold,
                               PngAlphaBounds& bounds);
bool crop_png_to_alpha_bounds(const PngImage& source, PngImage& cropped);
bool crop_png_to_alpha_threshold(const PngImage& source,
                                 unsigned char threshold, int padding,
                                 PngImage& cropped);
bool resize_png_bilinear(const PngImage& source, int width, int height,
                         PngImage& resized);
bool resize_png_lanczos3(const PngImage& source, int width, int height,
                         PngImage& resized,
                         PngResizeProfile* profile = 0);

}

#endif
