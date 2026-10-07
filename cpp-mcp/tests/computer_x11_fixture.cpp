#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <algorithm>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>

std::vector<unsigned long> window_property(Display* display, Window window, Atom property) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    const auto status = XGetWindowProperty(display, window, property, 0, 1024, False, XA_WINDOW, &type,
                                           &format, &count, &remaining, &data);
    std::vector<unsigned long> values;
    if (status == Success && type == XA_WINDOW && format == 32 && remaining == 0 && data) {
        const auto* first = reinterpret_cast<const unsigned long*>(data);
        values.assign(first, first + count);
    }
    if (data)
        XFree(data);
    return values;
}

// A real event receiver, controlled only through an owned pipe by the test harness.
int main(int argc, char** argv) {
    const bool require_wm = argc == 2 && std::string_view(argv[1]) == "--wait-for-wm";
    Display* display = XOpenDisplay(nullptr);
    if (!display)
        return 1;
    const auto root = DefaultRootWindow(display);
    const auto client_list = XInternAtom(display, "_NET_CLIENT_LIST", False);
    const auto window = XCreateSimpleWindow(display, root, 30, 30, 800, 600, 0, 0, 0xfafafa);
    XStoreName(display, window, "Devbox native input fixture");
    XWMHints hints{};
    hints.flags = InputHint | StateHint;
    hints.input = True;
    hints.initial_state = NormalState;
    XSetWMHints(display, window, &hints);
    XSizeHints size{};
    size.flags = USPosition | USSize;
    size.x = 30;
    size.y = 30;
    size.width = 800;
    size.height = 600;
    XSetWMNormalHints(display, window, &size);
    char instance[] = "devbox-fixture", app[] = "DevboxFixture";
    XClassHint klass{instance, app};
    XSetClassHint(display, window, &klass);
    const auto protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    const auto ping = XInternAtom(display, "_NET_WM_PING", False);
    Atom supported[]{ping, XInternAtom(display, "WM_DELETE_WINDOW", False)};
    XSetWMProtocols(display, window, supported, 2);
    const unsigned long pid = static_cast<unsigned long>(getpid());
    XChangeProperty(display, window, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL, 32,
                    PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
    XSelectInput(display, window,
                 KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     ExposureMask | StructureNotifyMask);
    // The harness maps after the optional window manager's startup hook has run.
    XSync(display, False);
    // Initialize Xlib's keyboard mapping before timed input arrives.
    (void)XKeysymToKeycode(display, 0x61);
    std::cout << "ready " << window << std::endl;
    Window overlay = 0;
    Window focus_target = 0;
    bool steal_ping = false;
    bool map_requested = false, mapped = false;
    for (;;) {
        // A map request acknowledgement alone does not prove the WM actually
        // mapped and registered the client. Inspect both on the same connection.
        if (map_requested && !mapped) {
            XWindowAttributes attributes{};
            const auto clients =
                require_wm ? window_property(display, root, client_list) : std::vector<unsigned long>{window};
            if (XGetWindowAttributes(display, window, &attributes) && attributes.map_state == IsViewable &&
                std::find(clients.begin(), clients.end(), window) != clients.end()) {
                mapped = true;
                std::cout << "mapped" << std::endl;
            }
        }
        while (XPending(display)) {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == ClientMessage && event.xclient.message_type == protocols &&
                static_cast<Atom>(event.xclient.data.l[0]) == ping) {
                if (steal_ping) {
                    steal_ping = false;
                    XSetInputFocus(display, focus_target, RevertToNone, CurrentTime);
                }
                event.xclient.window = root;
                XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
                XFlush(display);
            }
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
            if (command == "map") {
                XMapWindow(display, window);
                map_requested = true;
            }
            if (command == "steal_ping") {
                XSetWindowAttributes attr{};
                attr.override_redirect = True;
                focus_target = XCreateWindow(display, root, 900, 640, 100, 100, 0, CopyFromParent,
                                             InputOutput, CopyFromParent, CWOverrideRedirect, &attr);
                XSelectInput(display, focus_target, KeyPressMask | KeyReleaseMask);
                XMapRaised(display, focus_target);
                steal_ping = true;
            }
            if (command == "title")
                XStoreName(display, window, "Devbox changed fixture title");
            if (command == "resize")
                XResizeWindow(display, window, 720, 540);
            if (command == "cover" || command == "owned_popup") {
                XSetWindowAttributes attr{};
                attr.override_redirect = True;
                overlay = XCreateWindow(display, root, 60, 60, 300, 200, 0, CopyFromParent, InputOutput,
                                        CopyFromParent, CWOverrideRedirect, &attr);
                if (command == "owned_popup") {
                    XChangeProperty(display, overlay, XInternAtom(display, "_NET_WM_PID", False), XA_CARDINAL,
                                    32, PropModeReplace, reinterpret_cast<const unsigned char*>(&pid), 1);
                    XSetTransientForHint(display, overlay, window);
                    XSelectInput(display, overlay, ButtonPressMask | ButtonReleaseMask);
                }
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
