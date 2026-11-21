#ifndef GAMBITREE_DEFERRED_PIECE_RESIZE_H
#define GAMBITREE_DEFERRED_PIECE_RESIZE_H

#include <stdint.h>

#include "gambitree/piece_raster_worklist.h"

namespace gambitree {

class DeferredPieceResize {
public:
    enum State {
        IDLE,
        WAITING_FOR_SETTLE,
        BUILDING_CANDIDATE,
        READY_TO_COMMIT
    };

    explicit DeferredPieceResize(unsigned long settle_milliseconds)
        : settle_milliseconds_(settle_milliseconds), state_(IDLE),
          target_square_size_(0), last_event_milliseconds_(0),
          deadline_milliseconds_(0), generation_(0), deferred_targets_(0),
          cancelled_targets_(0), completed_targets_(0),
          total_settle_milliseconds_(0), maximum_settle_milliseconds_(0),
          settled_targets_(0), work_units_completed_(0) {}

    bool configure(int square_size, int high_quality_square_size,
                   uint64_t now_milliseconds)
    {
        if (square_size <= 0 || square_size == high_quality_square_size) {
            cancel();
            return false;
        }
        if ((state_ == BUILDING_CANDIDATE || state_ == READY_TO_COMMIT) &&
            target_square_size_ == square_size)
            return true;
        if (state_ != IDLE && target_square_size_ != square_size)
            ++cancelled_targets_;
        if (state_ == IDLE || target_square_size_ != square_size) {
            ++deferred_targets_;
            ++generation_;
            work_units_completed_ = 0;
        }
        state_ = WAITING_FOR_SETTLE;
        target_square_size_ = square_size;
        last_event_milliseconds_ = now_milliseconds;
        deadline_milliseconds_ = now_milliseconds + settle_milliseconds_;
        return true;
    }

    bool pending() const { return state_ != IDLE; }
    bool waiting_for_settle() const { return state_ == WAITING_FOR_SETTLE; }
    bool building() const { return state_ == BUILDING_CANDIDATE; }
    bool ready_to_commit() const { return state_ == READY_TO_COMMIT; }
    State state() const { return state_; }
    unsigned long work_units_completed() const
    {
        return work_units_completed_;
    }
    int target_square_size() const { return target_square_size_; }
    unsigned long generation() const { return generation_; }
    uint64_t deadline_milliseconds() const
    {
        return deadline_milliseconds_;
    }
    bool due(uint64_t now_milliseconds) const
    {
        return state_ == WAITING_FOR_SETTLE &&
               now_milliseconds >= deadline_milliseconds_;
    }
    uint64_t remaining_milliseconds(
        uint64_t now_milliseconds) const
    {
        return state_ != WAITING_FOR_SETTLE ||
               now_milliseconds >= deadline_milliseconds_
            ? 0 : deadline_milliseconds_ - now_milliseconds;
    }

    bool begin_build(uint64_t now_milliseconds, bool immediately = false)
    {
        if (state_ != WAITING_FOR_SETTLE ||
            (!immediately && !due(now_milliseconds)))
            return false;
        {
            uint64_t latency = now_milliseconds - last_event_milliseconds_;
            ++settled_targets_;
            total_settle_milliseconds_ += latency;
            if (latency > maximum_settle_milliseconds_)
                maximum_settle_milliseconds_ = latency;
        }
        state_ = BUILDING_CANDIDATE;
        work_units_completed_ = 0;
        return true;
    }

    bool finish_work_unit(bool all_work_complete)
    {
        if (state_ != BUILDING_CANDIDATE)
            return false;
        ++work_units_completed_;
        if (all_work_complete)
            state_ = READY_TO_COMMIT;
        return true;
    }

    bool commit(uint64_t)
    {
        if (state_ != READY_TO_COMMIT)
            return false;
        state_ = IDLE;
        ++completed_targets_;
        target_square_size_ = 0;
        deadline_milliseconds_ = 0;
        last_event_milliseconds_ = 0;
        work_units_completed_ = 0;
        return true;
    }

    void cancel()
    {
        if (state_ != IDLE) {
            ++cancelled_targets_;
            ++generation_;
        }
        state_ = IDLE;
        target_square_size_ = 0;
        deadline_milliseconds_ = 0;
        last_event_milliseconds_ = 0;
        work_units_completed_ = 0;
    }

    unsigned long deferred_targets() const { return deferred_targets_; }
    unsigned long cancelled_targets() const { return cancelled_targets_; }
    unsigned long completed_targets() const { return completed_targets_; }
    unsigned long settled_targets() const { return settled_targets_; }
    uint64_t total_settle_milliseconds() const
    {
        return total_settle_milliseconds_;
    }
    uint64_t maximum_settle_milliseconds() const
    {
        return maximum_settle_milliseconds_;
    }

private:
    unsigned long settle_milliseconds_;
    State state_;
    int target_square_size_;
    uint64_t last_event_milliseconds_;
    uint64_t deadline_milliseconds_;
    unsigned long generation_;
    unsigned long deferred_targets_;
    unsigned long cancelled_targets_;
    unsigned long completed_targets_;
    uint64_t total_settle_milliseconds_;
    uint64_t maximum_settle_milliseconds_;
    unsigned long settled_targets_;
    unsigned long work_units_completed_;
};

}

#endif
