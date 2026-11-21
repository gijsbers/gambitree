#include "gambitree/wheel_event_batch.h"

#include <climits>
#include <poll.h>

namespace gambitree {
namespace {
bool wheel_press(const XEvent& event, Window window)
{
    return event.type == ButtonPress && event.xbutton.window == window &&
           (event.xbutton.button == Button4 || event.xbutton.button == Button5);
}

bool wheel_release(const XEvent& event, Window window, unsigned int button)
{
    return event.type == ButtonRelease && event.xbutton.window == window &&
           event.xbutton.button == button;
}
}

int collect_contiguous_wheel_events(Display* display, Window window,
                                    const XEvent& first,
                                    unsigned long& event_count)
{
    long steps = 0;
    XEvent next;
    unsigned int last_button;
    event_count = 0;
    if (!display || !wheel_press(first, window)) return 0;
    last_button = first.xbutton.button;
    steps = last_button == Button4 ? -1 : 1;
    event_count = 1;
    while (true) {
        if (XPending(display) == 0) {
            struct pollfd descriptor;
            descriptor.fd = ConnectionNumber(display);
            descriptor.events = POLLIN;
            descriptor.revents = 0;
            /* A short collection window lets the release/next press from a
               physical wheel burst arrive together. It bounds added input
               latency, and any key, click or window event still ends the
               batch immediately without being consumed. */
            if (poll(&descriptor, 1, 4) <= 0 || XPending(display) == 0)
                break;
        }
        XPeekEvent(display, &next);
        if (wheel_release(next, window, last_button)) {
            XNextEvent(display, &next);
            continue;
        }
        if (!wheel_press(next, window)) break;
        XNextEvent(display, &next);
        last_button = next.xbutton.button;
        if (last_button == Button4) --steps;
        else ++steps;
        ++event_count;
        if (steps > INT_MAX) steps = INT_MAX;
        if (steps < INT_MIN) steps = INT_MIN;
    }
    if (steps > INT_MAX) return INT_MAX;
    if (steps < INT_MIN) return INT_MIN;
    return static_cast<int>(steps);
}

}
