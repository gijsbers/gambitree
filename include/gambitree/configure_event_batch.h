#ifndef GAMBITREE_CONFIGURE_EVENT_BATCH_H
#define GAMBITREE_CONFIGURE_EVENT_BATCH_H

#include <cstddef>

namespace gambitree {

/* Keeps the most recent size in a queue batch of ConfigureNotify events so
   the window can render once at the latest geometry. */
class ConfigureEventBatch {
public:
    ConfigureEventBatch() : width_(0), height_(0), event_count_(0) {}

    void include(int width, int height)
    {
        width_ = width;
        height_ = height;
        ++event_count_;
    }

    int width() const { return width_; }
    int height() const { return height_; }
    std::size_t event_count() const { return event_count_; }

private:
    int width_;
    int height_;
    std::size_t event_count_;
};

}

#endif
