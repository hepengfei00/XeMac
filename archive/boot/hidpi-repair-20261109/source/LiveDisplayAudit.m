#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <sys/sysctl.h>
#include <time.h>

static NSDictionary *describe(CGDisplayModeRef m) {
    if (!m) return @{};
    size_t w=CGDisplayModeGetWidth(m), h=CGDisplayModeGetHeight(m);
    size_t pw=CGDisplayModeGetPixelWidth(m), ph=CGDisplayModeGetPixelHeight(m);
    return @{@"logical_width":@(w), @"logical_height":@(h),
             @"pixel_width":@(pw), @"pixel_height":@(ph),
             @"refresh_hz":@(CGDisplayModeGetRefreshRate(m)),
             @"hidpi":(pw==2*w && ph==2*h ? @YES : @NO),
             @"mode_id":@(CGDisplayModeGetIODisplayModeID(m))};
}
int main(int argc, const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    NSString *output=nil, *nonce=nil;
    BOOL select=NO;
    for (int i=1; i<argc; i++) {
        if (!strcmp(argv[i],"--output") && i+1<argc) output=@(argv[++i]);
        else if (!strcmp(argv[i],"--nonce") && i+1<argc) nonce=@(argv[++i]);
        else if (!strcmp(argv[i],"--select-hidpi")) select=YES;
        else return 2;
    }
    if (!output || !nonce) return 2;
    char boot[128]={0}; size_t len=sizeof boot;
    if (sysctlbyname("kern.bootsessionuuid",boot,&len,NULL,0)) return 3;
    CGDirectDisplayID display=CGMainDisplayID();
    CFArrayRef modes=CGDisplayCopyAllDisplayModes(display,(__bridge CFDictionaryRef)
        @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes:@YES});
    NSMutableArray *available=[NSMutableArray new];
    CGError selection=kCGErrorSuccess;
    BOOL candidate=NO;
    if (modes) for (CFIndex i=0; i<CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m=(CGDisplayModeRef)CFArrayGetValueAtIndex(modes,i);
        [available addObject:describe(m)];
        double hz=CGDisplayModeGetRefreshRate(m);
        if (select && !candidate && CGDisplayModeGetWidth(m)==1920 &&
            CGDisplayModeGetHeight(m)==1080 && CGDisplayModeGetPixelWidth(m)==3840 &&
            CGDisplayModeGetPixelHeight(m)==2160 && hz>59 && hz<61) {
            candidate=YES;
            selection=CGDisplaySetDisplayMode(display,m,NULL);
        }
    }
    if (modes) CFRelease(modes);
    if (select && candidate && selection==kCGErrorSuccess) {
        for (int attempt=0; attempt<50; attempt++) {
            CGDisplayModeRef check=CGDisplayCopyDisplayMode(display);
            BOOL ready=check && CGDisplayModeGetWidth(check)==1920 &&
                CGDisplayModeGetHeight(check)==1080 && CGDisplayModeGetPixelWidth(check)==3840 &&
                CGDisplayModeGetPixelHeight(check)==2160;
            if (check) CGDisplayModeRelease(check);
            if (ready) break;
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        }
    }
    CGDisplayModeRef current=CGDisplayCopyDisplayMode(display);
    NSDictionary *result=@{@"schema":@1, @"boot_uuid":@(boot), @"nonce":nonce,
        @"generated_unix":@((long long)time(NULL)), @"pid":@(getpid()),
        @"display_id":@(display), @"online":(CGDisplayIsOnline(display) ? @YES : @NO),
        @"selection_requested":(select ? @YES : @NO), @"selection_candidate":(candidate ? @YES : @NO),
        @"selection_rc":@(selection), @"current":describe(current), @"modes":available};
    if (current) CGDisplayModeRelease(current);
    NSData *data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
    return [data writeToFile:output options:NSDataWritingAtomic error:nil] ? 0 : 4;
}}
