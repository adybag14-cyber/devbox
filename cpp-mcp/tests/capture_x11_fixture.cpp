#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <csignal>
#include <iostream>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stop = 0;
void interrupted(int) {
    stop = 1;
}
} // namespace
int main() {
    std::signal(SIGTERM, interrupted);
    std::signal(SIGINT, interrupted);
    auto* display = XOpenDisplay(nullptr);
    if (!display)
        return 2;
    const auto screen = DefaultScreen(display);
    const auto parent = RootWindow(display, screen);
    const auto pid_key = XInternAtom(display, "_NET_WM_PID", False);
    const unsigned long pid = static_cast<unsigned long>(getpid());
    auto create = [&](int x, int y, unsigned width, unsigned height) {
        const auto window = XCreateSimpleWindow(display, parent, x, y, width, height, 0, 0, 0);
        XStoreName(display, window, "Devbox owned X11 capture fixture");
        XChangeProperty(display, window, pid_key, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&pid), 1);
        XSelectInput(display, window, ExposureMask);
        XMapWindow(display, window);
        return window;
    };
    const auto small = create(500, 300, 120, 90);
    const auto large = create(80, 80, 360, 240);
    const auto hidden = create(0, 0, 700, 500);
    XUnmapWindow(display, hidden);
    const auto gc = XCreateGC(display, parent, 0, nullptr);
    auto paint = [&](Window window, unsigned width, unsigned height) {
        XSetForeground(display, gc, 0xE6642D);
        XFillRectangle(display, window, gc, 0, 0, width / 2, height);
        XSetForeground(display, gc, 0x2878E6);
        XFillRectangle(display, window, gc, static_cast<int>(width / 2), 0, width - width / 2, height);
    };
    paint(small, 120, 90);
    paint(large, 360, 240);
    XSync(display, False);
    std::cout << "ready " << pid << '\n' << std::flush;
    while (!stop) {
        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == Expose) {
                if (event.xexpose.window == small)
                    paint(small, 120, 90);
                if (event.xexpose.window == large)
                    paint(large, 360, 240);
                XFlush(display);
            }
        }
        usleep(10000);
    }
    XFreeGC(display, gc);
    for (const auto window : {small, large, hidden})
        XDestroyWindow(display, window);
    XCloseDisplay(display);
}
