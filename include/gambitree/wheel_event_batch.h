#ifndef GAMBITREE_WHEEL_EVENT_BATCH_H
#define GAMBITREE_WHEEL_EVENT_BATCH_H

#include <X11/Xlib.h>

namespace gambitree {

/* Coalesces immediately adjacent wheel presses for the same client window,
   consuming the matching ButtonRelease events emitted by X11 devices too.
   Any other event remains in the queue and preserves its original order. */
int collect_contiguous_wheel_events(Display* display, Window window,
                                    const XEvent& first,
                                    unsigned long& event_count);

}

#endif
