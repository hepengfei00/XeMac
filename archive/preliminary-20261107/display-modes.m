#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
static NSDictionary *modeInfo(CGDisplayModeRef mode) {
    if(!mode)return @{};
    size_t w=CGDisplayModeGetWidth(mode),h=CGDisplayModeGetHeight(mode);
    size_t pw=CGDisplayModeGetPixelWidth(mode),ph=CGDisplayModeGetPixelHeight(mode);
    return @{@"id":@(CGDisplayModeGetIODisplayModeID(mode)),@"logical_width":@(w),@"logical_height":@(h),@"pixel_width":@(pw),@"pixel_height":@(ph),@"refresh_hz":@(CGDisplayModeGetRefreshRate(mode)),@"hidpi":@(pw>w&&ph>h)};
}
int main(void){@autoreleasepool{
    CGDirectDisplayID ids[16];uint32_t count=0;
    if(CGGetActiveDisplayList(16,ids,&count)!=kCGErrorSuccess)return 2;
    NSMutableArray *displays=[NSMutableArray new];
    for(uint32_t i=0;i<count;i++){
        CGDisplayModeRef current=CGDisplayCopyDisplayMode(ids[i]);
        CFArrayRef modes=CGDisplayCopyAllDisplayModes(ids[i],(__bridge CFDictionaryRef)@{(__bridge NSString*)kCGDisplayShowDuplicateLowResolutionModes:@YES});
        NSMutableArray *all=[NSMutableArray new];
        if(modes)for(CFIndex k=0;k<CFArrayGetCount(modes);k++)[all addObject:modeInfo((CGDisplayModeRef)CFArrayGetValueAtIndex(modes,k))];
        [displays addObject:@{@"display_id":@(ids[i]),@"current":modeInfo(current),@"modes":all}];
        if(current)CGDisplayModeRelease(current);if(modes)CFRelease(modes);
    }
    NSData *data=[NSJSONSerialization dataWithJSONObject:displays options:NSJSONWritingPrettyPrinted error:nil];fwrite(data.bytes,1,data.length,stdout);putchar('\n');return 0;
}}
