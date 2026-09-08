#import <Cocoa/Cocoa.h>
#include <csignal>
#include <iostream>
#include <string>
#include <unistd.h>
namespace {
volatile std::sig_atomic_t stop = 0;
void interrupted(int) {
    stop = 1;
}
} // namespace
@interface DevboxFixtureView : NSView
@end
@implementation DevboxFixtureView
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect left = self.bounds;
    left.size.width /= 2;
    [[NSColor colorWithSRGBRed:230.0 / 255 green:100.0 / 255 blue:45.0 / 255 alpha:1] setFill];
    NSRectFill(left);
    NSRect right = self.bounds;
    right.origin.x = left.size.width;
    right.size.width -= left.size.width;
    [[NSColor colorWithSRGBRed:40.0 / 255 green:120.0 / 255 blue:230.0 / 255 alpha:1] setFill];
    NSRectFill(right);
}
@end
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc == 3 && std::string(argv[1]) == "--check-image") {
            NSBitmapImageRep* bitmap =
                [NSBitmapImageRep imageRepWithContentsOfFile:[NSString stringWithUTF8String:argv[2]]];
            if (!bitmap || bitmap.pixelsWide < 100 || bitmap.pixelsHigh < 100)
                return 3;
            NSColor* left = [[bitmap colorAtX:bitmap.pixelsWide / 4 y:bitmap.pixelsHigh / 2]
                colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];
            NSColor* right = [[bitmap colorAtX:bitmap.pixelsWide * 3 / 4 y:bitmap.pixelsHigh / 2]
                colorUsingColorSpace:[NSColorSpace deviceRGBColorSpace]];
            if (!left || !right || left.redComponent < right.redComponent + 0.2 ||
                right.blueComponent < left.blueComponent + 0.2)
                return 4;
            std::cout << "{\"width\":" << bitmap.pixelsWide << ",\"height\":" << bitmap.pixelsHigh
                      << ",\"color_pattern\":true}\n";
            return 0;
        }
        std::signal(SIGTERM, interrupted);
        std::signal(SIGINT, interrupted);
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        auto window = [](CGFloat x, CGFloat y, CGFloat width, CGFloat height) {
            NSWindow* value = [[NSWindow alloc] initWithContentRect:NSMakeRect(x, y, width, height)
                                                          styleMask:NSWindowStyleMaskBorderless
                                                            backing:NSBackingStoreBuffered
                                                              defer:NO];
            value.title = @"Devbox owned Cocoa capture fixture";
            value.releasedWhenClosed = NO;
            value.opaque = YES;
            value.contentView = [[DevboxFixtureView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
            return value;
        };
        NSWindow* small = window(600, 200, 120, 90);
        NSWindow* large = window(80, 80, 360, 240);
        NSWindow* hidden = window(0, 0, 700, 500);
        [small orderFrontRegardless];
        [large orderFrontRegardless];
        [small display];
        [large display];
        [app updateWindows];
        std::cout << "ready " << getpid() << '\n' << std::flush;
        while (!stop) {
            @autoreleasepool {
                NSEvent* event = [app nextEventMatchingMask:NSEventMaskAny
                                                  untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                                     inMode:NSDefaultRunLoopMode
                                                    dequeue:YES];
                if (event)
                    [app sendEvent:event];
                [app updateWindows];
            }
        }
        [small close];
        [large close];
        [hidden close];
    }
}
