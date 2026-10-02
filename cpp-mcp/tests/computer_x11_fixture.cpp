#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

// A real event receiver, controlled only through an owned pipe by the test harness.
int main() {
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return 1;
    const auto root = DefaultRootWindow(display);
    const auto window = XCreateSimpleWindow(display, root, 30, 30, 800, 600, 0, 0, 0xfafafa);
    XStoreName(display, window, "Devbox native input fixture");
    const unsigned long pid = static_cast<unsigned long>(getpid());
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
    XSelectInput(display, window,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     ExposureMask | StructureNotifyMask);
    XMapWindow(display, window);
    XFlush(display);
    std::cout << "ready " << window << std::endl;
    Window overlay = 0;
    for (;;) {
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == MappingNotify)
                XRefreshKeyboardMapping(&event.xmapping);
            if (event.type == KeyPress || event.type == KeyRelease) {
                char text[32];
                KeySym symbol = 0;
                XLookupString(&event.xkey, text, sizeof(text), &symbol, nullptr);
                std::cout << (event.type == KeyPress ? "key_down " : "key_up ") << symbol << std::endl;
            }
            if (event.type == ButtonPress || event.type == ButtonRelease)
                std::cout << (event.type == ButtonPress ? "button_down " : "button_up ")
                          << event.xbutton.button << std::endl;
            if (event.type == MotionNotify)
                std::cout << "motion " << event.xmotion.x << ' ' << event.xmotion.y << std::endl;
            if (event.type == Expose) {
                GC gc = XCreateGC(display, window, 0, nullptr);
                XSetForeground(display, gc, 0x2978e6);
                XFillRectangle(display, window, gc, 0, 0, 400, 600);
                XSetForeground(display, gc, 0xe6642d);
                XFillRectangle(display, window, gc, 400, 0, 400, 600);
                XFreeGC(display, gc);
                XFlush(display);
            }
        }
        pollfd fds[2]{{ConnectionNumber(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (poll(fds, 2, 50) < 0)
            break;
        if (fds[1].revents & (POLLHUP | POLLERR))
            break;
        if (fds[1].revents & POLLIN) {
            std::string command;
            if (!std::getline(std::cin, command))
                break;
            if (command == "quit")
                break;
            if (command == "title")
                XStoreName(display, window, "Devbox changed fixture title");
            if (command == "resize")
                XResizeWindow(display, window, 720, 540);
            if (command == "cover") {
                XSetWindowAttributes attr{};
                attr.override_redirect = True;
                overlay = XCreateWindow(display, root, 60, 60, 300, 200, 0, CopyFromParent, InputOutput,
                                        CopyFromParent, CWOverrideRedirect, &attr);
                XMapRaised(display, overlay);
            }
            if (command == "uncover" && overlay) {
                XDestroyWindow(display, overlay);
                overlay = 0;
            }
            XSync(display, False);
            std::cout << "ack " << command << std::endl;
        }
    }
    XDestroyWindow(display, window);
    XCloseDisplay(display);
}
