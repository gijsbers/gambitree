#include "gambitree/png_image.h"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <sys/time.h>
#include <vector>

namespace gambitree {

namespace {

double elapsed_milliseconds(const timeval& start, const timeval& end)
{
    return (end.tv_sec - start.tv_sec) * 1000.0 +
           (end.tv_usec - start.tv_usec) / 1000.0;
}

bool checked_product(std::size_t left, std::size_t right,
                     std::size_t& result)
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
        return false;
    result = left * right;
    return true;
}

double lanczos_weight(double distance, double scale)
{
    const double pi = 3.14159265358979323846;
    double normalized = distance / scale;
    if (std::fabs(normalized) < 1e-12)
        return 1.0;
    if (std::fabs(normalized) >= 3.0)
        return 0.0;
    return (std::sin(pi * normalized) / (pi * normalized)) *
           (std::sin(pi * normalized / 3.0) /
            (pi * normalized / 3.0));
}

unsigned char clamp_channel(double value)
{
    if (value <= 0.0)
        return 0;
    if (value >= 255.0)
        return 255;
    return static_cast<unsigned char>(value + 0.5);
}

struct ResampleTap {
    int source;
    double weight;
};

typedef std::vector<ResampleTap> ResampleFilter;

bool build_resample_filter(int source_size, int destination_size,
                           std::vector<ResampleFilter>& filters)
{
    double scale = std::max(1.0,
        static_cast<double>(source_size) / destination_size);
    int destination;
    filters.resize(destination_size);
    for (destination = 0; destination < destination_size; ++destination) {
        double center = (destination + 0.5) * source_size /
                        destination_size - 0.5;
        int first = static_cast<int>(std::ceil(center - 3.0 * scale));
        int last = static_cast<int>(std::floor(center + 3.0 * scale));
        double total_weight = 0.0;
        int source;
        if (first < 0)
            first = 0;
        if (last >= source_size)
            last = source_size - 1;
        for (source = first; source <= last; ++source) {
            ResampleTap tap;
            tap.source = source;
            tap.weight = lanczos_weight(source - center, scale);
            if (tap.weight != 0.0) {
                filters[destination].push_back(tap);
                total_weight += tap.weight;
            }
        }
        if (std::fabs(total_weight) < 1e-12)
            return false;
        for (std::size_t tap_index = 0;
             tap_index < filters[destination].size(); ++tap_index)
            filters[destination][tap_index].weight /= total_weight;
    }
    return true;
}

void png_error_handler(png_structp png, png_const_charp)
{
    longjmp(png_jmpbuf(png), 1);
}

}

PngImage::PngImage() : width(0), height(0), stride(0) {}
bool PngImage::valid() const
{
    if (width <= 0 || height <= 0 ||
        stride < static_cast<std::size_t>(width) * 4 ||
        stride > std::numeric_limits<std::size_t>::max() /
                     static_cast<std::size_t>(height))
        return false;
    return rgba.size() >= stride * static_cast<std::size_t>(height);
}

PngAlphaBounds::PngAlphaBounds() : x(0), y(0), width(0), height(0) {}
bool PngAlphaBounds::valid() const { return width > 0 && height > 0; }
PngResizeProfile::PngResizeProfile() : horizontal_ms(0.0), vertical_ms(0.0) {}

bool load_png_rgba(const std::string& path, PngImage& image, std::string& error)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    png_structp png;
    png_infop info;
    std::vector<png_bytep> rows;
    png_uint_32 width;
    png_uint_32 height;
    int bit_depth;
    int color_type;
    if (!file) { error = "cannot open PNG '" + path + "'"; return false; }
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, 0, png_error_handler, 0);
    if (!png) { std::fclose(file); error = "cannot create PNG reader"; return false; }
    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, 0, 0); std::fclose(file); error = "cannot create PNG metadata"; return false; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, 0);
        std::fclose(file);
        image = PngImage();
        error = "invalid PNG '" + path + "'";
        return false;
    }
    png_init_io(png, file);
    png_read_info(png, info);
    width = png_get_image_width(png, info);
    height = png_get_image_height(png, info);
    bit_depth = png_get_bit_depth(png, info);
    color_type = png_get_color_type(png, info);
    if (!width || !height || width > 4096 || height > 4096) png_error(png, "invalid dimensions");
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) png_set_filler(png, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(png, info);
    image.width = static_cast<int>(width);
    image.height = static_cast<int>(height);
    image.stride = png_get_rowbytes(png, info);
    if (image.stride < static_cast<std::size_t>(width) * 4) png_error(png, "invalid RGBA row stride");
    image.rgba.resize(image.stride * height);
    rows.resize(height);
    for (png_uint_32 row = 0; row < height; ++row)
        rows[row] = reinterpret_cast<png_bytep>(&image.rgba[0]) + row * image.stride;
    png_read_image(png, &rows[0]);
    png_read_end(png, 0);
    png_destroy_read_struct(&png, &info, 0);
    std::fclose(file);
    error.clear();
    return true;
}

bool png_alpha_bounds_at_least(const PngImage& image, unsigned char threshold,
                               PngAlphaBounds& bounds)
{
    int minimum_x;
    int minimum_y;
    int maximum_x = -1;
    int maximum_y = -1;
    int y;
    if (!image.valid())
        return false;
    minimum_x = image.width;
    minimum_y = image.height;
    for (y = 0; y < image.height; ++y) {
        int x;
        const unsigned char* row = &image.rgba[
            static_cast<std::size_t>(y) * image.stride];
        for (x = 0; x < image.width; ++x) {
            if (row[static_cast<std::size_t>(x) * 4 + 3] < threshold)
                continue;
            if (x < minimum_x)
                minimum_x = x;
            if (x > maximum_x)
                maximum_x = x;
            if (y < minimum_y)
                minimum_y = y;
            if (y > maximum_y)
                maximum_y = y;
        }
    }
    if (maximum_x < minimum_x || maximum_y < minimum_y)
        return false;
    bounds.x = minimum_x;
    bounds.y = minimum_y;
    bounds.width = maximum_x - minimum_x + 1;
    bounds.height = maximum_y - minimum_y + 1;
    return true;
}

bool png_alpha_bounds(const PngImage& image, PngAlphaBounds& bounds)
{
    return png_alpha_bounds_at_least(image, 1, bounds);
}

bool crop_png_to_alpha_bounds(const PngImage& source, PngImage& cropped)
{
    PngAlphaBounds bounds;
    std::size_t row_bytes;
    std::size_t total_bytes;
    int y;
    if (!png_alpha_bounds(source, bounds))
        return false;
    cropped.width = bounds.width;
    cropped.height = bounds.height;
    if (!checked_product(static_cast<std::size_t>(cropped.width), 4U,
                         row_bytes) ||
        !checked_product(row_bytes, static_cast<std::size_t>(cropped.height),
                         total_bytes))
        return false;
    cropped.stride = row_bytes;
    try {
        cropped.rgba.resize(total_bytes);
    } catch (const std::bad_alloc&) {
        cropped = PngImage();
        return false;
    }
    for (y = 0; y < cropped.height; ++y) {
        const unsigned char* source_row = &source.rgba[
            static_cast<std::size_t>(bounds.y + y) * source.stride +
            static_cast<std::size_t>(bounds.x) * 4];
        unsigned char* destination_row = &cropped.rgba[
            static_cast<std::size_t>(y) * cropped.stride];
        std::memcpy(destination_row, source_row, row_bytes);
    }
    return true;
}

bool crop_png_to_alpha_threshold(const PngImage& source,
                                 unsigned char threshold, int padding,
                                 PngImage& cropped)
{
    PngAlphaBounds bounds;
    PngImage threshold_crop;
    std::size_t row_bytes;
    std::size_t total_bytes;
    int left;
    int top;
    int right;
    int bottom;
    int y;
    if (threshold == 0 || padding < 0 ||
        !png_alpha_bounds_at_least(source, threshold, bounds))
        return false;
    left = std::max(0, bounds.x - padding);
    top = std::max(0, bounds.y - padding);
    right = std::min(source.width, bounds.x + bounds.width + padding);
    bottom = std::min(source.height, bounds.y + bounds.height + padding);
    threshold_crop.width = right - left;
    threshold_crop.height = bottom - top;
    if (!checked_product(static_cast<std::size_t>(threshold_crop.width), 4U,
                         row_bytes) ||
        !checked_product(row_bytes,
                         static_cast<std::size_t>(threshold_crop.height),
                         total_bytes))
        return false;
    threshold_crop.stride = row_bytes;
    try {
        threshold_crop.rgba.resize(total_bytes);
    } catch (const std::bad_alloc&) {
        return false;
    }
    for (y = 0; y < threshold_crop.height; ++y) {
        const unsigned char* source_row = &source.rgba[
            static_cast<std::size_t>(top + y) * source.stride +
            static_cast<std::size_t>(left) * 4];
        unsigned char* target_row = &threshold_crop.rgba[
            static_cast<std::size_t>(y) * threshold_crop.stride];
        std::memcpy(target_row, source_row, row_bytes);
    }
    cropped.width = threshold_crop.width;
    cropped.height = threshold_crop.height;
    cropped.stride = threshold_crop.stride;
    cropped.rgba.swap(threshold_crop.rgba);
    return true;
}

bool resize_png_bilinear(const PngImage& source, int width, int height,
                         PngImage& resized)
{
    const int maximum_dimension = 4096;
    std::size_t output_stride;
    std::size_t output_bytes;
    int y;
    if (!source.valid() || width <= 0 || height <= 0 ||
        width > maximum_dimension || height > maximum_dimension ||
        !checked_product(static_cast<std::size_t>(width), 4U, output_stride) ||
        !checked_product(output_stride, static_cast<std::size_t>(height),
                         output_bytes))
        return false;
    if (width == source.width && height == source.height) {
        try {
            resized = source;
        } catch (const std::bad_alloc&) {
            resized = PngImage();
            return false;
        }
        return true;
    }
    try {
        resized.width = width;
        resized.height = height;
        resized.stride = output_stride;
        resized.rgba.resize(output_bytes);
    } catch (const std::bad_alloc&) {
        resized = PngImage();
        return false;
    }

    for (y = 0; y < height; ++y) {
        double source_y = (static_cast<double>(y) + 0.5) *
                          source.height / height - 0.5;
        int y0 = static_cast<int>(source_y);
        int y1;
        double fy;
        int x;
        if (y0 < 0)
            y0 = 0;
        if (y0 >= source.height)
            y0 = source.height - 1;
        y1 = y0 + 1 < source.height ? y0 + 1 : y0;
        fy = source_y - (source_y < 0.0 ? source_y : static_cast<int>(source_y));
        if (source_y < 0.0)
            fy = 0.0;
        if (fy > 1.0)
            fy = 1.0;
        for (x = 0; x < width; ++x) {
            double source_x = (static_cast<double>(x) + 0.5) *
                              source.width / width - 0.5;
            int x0 = static_cast<int>(source_x);
            int x1;
            double fx;
            const unsigned char* p00;
            const unsigned char* p10;
            const unsigned char* p01;
            const unsigned char* p11;
            double weights[4];
            const unsigned char* samples[4];
            double alpha = 0.0;
            double premultiplied[3] = { 0.0, 0.0, 0.0 };
            int sample;
            std::size_t destination = static_cast<std::size_t>(y) *
                                      output_stride +
                                      static_cast<std::size_t>(x) * 4;
            if (x0 < 0)
                x0 = 0;
            if (x0 >= source.width)
                x0 = source.width - 1;
            x1 = x0 + 1 < source.width ? x0 + 1 : x0;
            fx = source_x - (source_x < 0.0 ? source_x : static_cast<int>(source_x));
            if (source_x < 0.0)
                fx = 0.0;
            if (fx > 1.0)
                fx = 1.0;
            p00 = &source.rgba[static_cast<std::size_t>(y0) *
                               source.stride + static_cast<std::size_t>(x0) * 4];
            p10 = &source.rgba[static_cast<std::size_t>(y0) *
                               source.stride + static_cast<std::size_t>(x1) * 4];
            p01 = &source.rgba[static_cast<std::size_t>(y1) *
                               source.stride + static_cast<std::size_t>(x0) * 4];
            p11 = &source.rgba[static_cast<std::size_t>(y1) *
                               source.stride + static_cast<std::size_t>(x1) * 4];
            samples[0] = p00;
            samples[1] = p10;
            samples[2] = p01;
            samples[3] = p11;
            weights[0] = (1.0 - fx) * (1.0 - fy);
            weights[1] = fx * (1.0 - fy);
            weights[2] = (1.0 - fx) * fy;
            weights[3] = fx * fy;
            for (sample = 0; sample < 4; ++sample) {
                double sample_alpha = samples[sample][3] / 255.0;
                int channel;
                alpha += sample_alpha * weights[sample];
                for (channel = 0; channel < 3; ++channel)
                    premultiplied[channel] +=
                        samples[sample][channel] * sample_alpha *
                        weights[sample];
            }
            if (alpha <= 0.0) {
                resized.rgba[destination] = 0;
                resized.rgba[destination + 1] = 0;
                resized.rgba[destination + 2] = 0;
                resized.rgba[destination + 3] = 0;
            } else {
                int channel;
                for (channel = 0; channel < 3; ++channel) {
                    double value = premultiplied[channel] / alpha;
                    if (value < 0.0)
                        value = 0.0;
                    if (value > 255.0)
                        value = 255.0;
                    resized.rgba[destination + channel] =
                        static_cast<unsigned char>(value + 0.5);
                }
                if (alpha > 1.0)
                    alpha = 1.0;
                resized.rgba[destination + 3] =
                    static_cast<unsigned char>(alpha * 255.0 + 0.5);
            }
        }
    }
    return true;
}

struct PngLanczos3Job::Impl {
    const PngImage* source;
    PngImage output;
    std::vector<double> horizontal;
    std::vector<ResampleFilter> horizontal_filter;
    std::vector<ResampleFilter> vertical_filter;
    PngResizeProfile resize_profile;
    int destination_width;
    int destination_height;
    int next_row;
    std::size_t output_stride;
    bool vertical_pass;
    bool complete;
    timeval pass_started;

    Impl()
        : source(0), destination_width(0), destination_height(0), next_row(0),
          output_stride(0), vertical_pass(false), complete(false), pass_started()
    {
    }

    void process_horizontal_row(int y)
    {
        int destination_x;
        const unsigned char* source_row = &source->rgba[
            static_cast<std::size_t>(y) * source->stride];
        for (destination_x = 0; destination_x < destination_width;
             ++destination_x) {
            double channels[4] = { 0.0, 0.0, 0.0, 0.0 };
            const ResampleFilter& filter = horizontal_filter[destination_x];
            std::size_t tap_index;
            for (tap_index = 0; tap_index < filter.size(); ++tap_index) {
                const ResampleTap& tap = filter[tap_index];
                const unsigned char* pixel = source_row +
                    static_cast<std::size_t>(tap.source) * 4;
                double alpha = pixel[3] / 255.0;
                channels[0] += pixel[0] * alpha * tap.weight;
                channels[1] += pixel[1] * alpha * tap.weight;
                channels[2] += pixel[2] * alpha * tap.weight;
                channels[3] += pixel[3] * tap.weight;
            }
            std::size_t offset =
                (static_cast<std::size_t>(y) * destination_width +
                 destination_x) * 4;
            int channel;
            for (channel = 0; channel < 4; ++channel)
                horizontal[offset + channel] = channels[channel];
        }
    }

    void process_vertical_row(int y)
    {
        int x;
        const ResampleFilter& filter = vertical_filter[y];
        for (x = 0; x < destination_width; ++x) {
            double channels[4] = { 0.0, 0.0, 0.0, 0.0 };
            std::size_t tap_index;
            for (tap_index = 0; tap_index < filter.size(); ++tap_index) {
                const ResampleTap& tap = filter[tap_index];
                std::size_t offset =
                    (static_cast<std::size_t>(tap.source) *
                     destination_width + x) * 4;
                int channel;
                for (channel = 0; channel < 4; ++channel)
                    channels[channel] += horizontal[offset + channel] *
                                         tap.weight;
            }
            double alpha = std::max(0.0, std::min(255.0, channels[3]));
            std::size_t offset = static_cast<std::size_t>(y) * output_stride +
                                 static_cast<std::size_t>(x) * 4;
            if (alpha > 1e-8) {
                double red = std::max(0.0, std::min(alpha, channels[0]));
                double green = std::max(0.0, std::min(alpha, channels[1]));
                double blue = std::max(0.0, std::min(alpha, channels[2]));
                output.rgba[offset] = clamp_channel(red * 255.0 / alpha);
                output.rgba[offset + 1] = clamp_channel(
                    green * 255.0 / alpha);
                output.rgba[offset + 2] = clamp_channel(
                    blue * 255.0 / alpha);
            } else {
                output.rgba[offset] = 0;
                output.rgba[offset + 1] = 0;
                output.rgba[offset + 2] = 0;
            }
            output.rgba[offset + 3] = clamp_channel(alpha);
        }
    }
};

PngLanczos3Job::PngLanczos3Job() : impl_(0) {}

PngLanczos3Job::~PngLanczos3Job()
{
    delete impl_;
}

bool PngLanczos3Job::start(const PngImage& source, int width, int height)
{
    const int maximum_dimension = 4096;
    std::size_t temporary_pixels;
    std::size_t temporary_values;
    std::size_t output_stride;
    std::size_t output_bytes;
    Impl* job = 0;
    cancel();
    if (!source.valid() || width <= 0 || height <= 0 ||
        width > maximum_dimension || height > maximum_dimension ||
        !checked_product(static_cast<std::size_t>(width),
                         static_cast<std::size_t>(source.height),
                         temporary_pixels) ||
        !checked_product(temporary_pixels, 4U, temporary_values) ||
        !checked_product(static_cast<std::size_t>(width), 4U, output_stride) ||
        !checked_product(output_stride, static_cast<std::size_t>(height),
                         output_bytes))
        return false;
    try {
        job = new Impl;
        job->source = &source;
        job->destination_width = width;
        job->destination_height = height;
        job->output_stride = output_stride;
        if (width == source.width && height == source.height) {
            job->output = source;
            job->complete = true;
            impl_ = job;
            return true;
        }
        if (!build_resample_filter(source.width, width,
                                   job->horizontal_filter) ||
            !build_resample_filter(source.height, height,
                                   job->vertical_filter)) {
            delete job;
            return false;
        }
        job->horizontal.resize(temporary_values);
        job->output.width = width;
        job->output.height = height;
        job->output.stride = output_stride;
        job->output.rgba.resize(output_bytes);
        gettimeofday(&job->pass_started, 0);
    } catch (const std::bad_alloc&) {
        delete job;
        return false;
    }
    impl_ = job;
    return true;
}

bool PngLanczos3Job::step(std::size_t maximum_rows)
{
    std::size_t rows_done = 0;
    if (!impl_ || impl_->complete)
        return ready();
    if (maximum_rows == 0)
        maximum_rows = 1;
    while (rows_done < maximum_rows && !impl_->complete) {
        if (!impl_->vertical_pass) {
            impl_->process_horizontal_row(impl_->next_row++);
            ++rows_done;
            if (impl_->next_row == impl_->source->height) {
                timeval finished;
                gettimeofday(&finished, 0);
                impl_->resize_profile.horizontal_ms = elapsed_milliseconds(
                    impl_->pass_started, finished);
                impl_->vertical_pass = true;
                impl_->next_row = 0;
                gettimeofday(&impl_->pass_started, 0);
            }
        } else {
            impl_->process_vertical_row(impl_->next_row++);
            ++rows_done;
            if (impl_->next_row == impl_->destination_height) {
                timeval finished;
                gettimeofday(&finished, 0);
                impl_->resize_profile.vertical_ms = elapsed_milliseconds(
                    impl_->pass_started, finished);
                impl_->complete = true;
            }
        }
    }
    return impl_->complete;
}

bool PngLanczos3Job::ready() const
{
    return impl_ && impl_->complete;
}

const PngImage& PngLanczos3Job::result() const
{
    static const PngImage empty;
    return impl_ ? impl_->output : empty;
}

PngResizeProfile PngLanczos3Job::profile() const
{
    return impl_ ? impl_->resize_profile : PngResizeProfile();
}

void PngLanczos3Job::take_result(PngImage& result)
{
    if (!ready()) {
        result = PngImage();
        return;
    }
    result.width = impl_->output.width;
    result.height = impl_->output.height;
    result.stride = impl_->output.stride;
    result.rgba.swap(impl_->output.rgba);
}

void PngLanczos3Job::cancel()
{
    delete impl_;
    impl_ = 0;
}

bool resize_png_lanczos3(const PngImage& source, int width, int height,
                         PngImage& resized, PngResizeProfile* profile)
{
    PngLanczos3Job job;
    if (!job.start(source, width, height)) {
        resized = PngImage();
        return false;
    }
    while (!job.ready())
        job.step(64);
    job.take_result(resized);
    if (profile)
        *profile = job.profile();
    return resized.valid();
}

}
