#ifndef GAMBITREE_COMPLETE_FRAME_BUFFER_H
#define GAMBITREE_COMPLETE_FRAME_BUFFER_H

namespace gambitree {

/* Tracks only fully rendered, application-owned frame targets. */
class CompleteFrameBuffer {
public:
    CompleteFrameBuffer(unsigned long drawable, int width, int height)
        : drawable_(drawable), width_(width), height_(height)
    {
    }

    bool needs_replacement(int width, int height) const
    {
        return drawable_ == 0 || width_ != width || height_ != height;
    }

    bool valid() const
    {
        return drawable_ != 0 && width_ > 0 && height_ > 0;
    }

    unsigned long drawable() const { return drawable_; }
    unsigned long drawable_for_expose() const
    {
        return valid() ? drawable_ : 0;
    }
    int width() const { return width_; }
    int height() const { return height_; }

    bool commit_completed(unsigned long drawable, int width, int height,
                          bool fully_rendered)
    {
        if (drawable == 0 || width <= 0 || height <= 0 || !fully_rendered)
            return false;
        drawable_ = drawable;
        width_ = width;
        height_ = height;
        return true;
    }

private:
    unsigned long drawable_;
    int width_;
    int height_;
};

} // namespace gambitree

#endif
