#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <IOKit/IOKitLib.h>
#import <ImageIO/ImageIO.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <math.h>

int main(int argc,const char **argv) { @autoreleasepool {
    if(argc!=2){fprintf(stderr,"Usage: ca-shape-probe output-prefix\n");return 2;}
    id<MTLDevice> dev=nil;const char *plugin=getenv("NM_TEST_PLUGIN");
    if(plugin){
        if(!dlopen(plugin,RTLD_NOW|RTLD_LOCAL)){fprintf(stderr,"%s\n",dlerror());return 2;}
        io_service_t s=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NMXAccel"));
        if(s){dev=((id (*)(id,SEL,mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],NSSelectorFromString(@"initWithAcceleratorPort:"),s);IOObjectRelease(s);}
    }else dev=MTLCreateSystemDefaultDevice();
    if(!dev){fprintf(stderr,"No requested device.\n");return 2;}
    enum{W=512,H=256,Pitch=W*4};
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;
    id<MTLTexture> image=[dev newTextureWithDescriptor:td];id<MTLCommandQueue> queue=[dev newCommandQueue];
    id<MTLBuffer> pixels=[dev newBufferWithLength:Pitch*H options:MTLResourceStorageModeShared];
    if(!image||!queue||!pixels){fprintf(stderr,"Resource creation failed.\n");return 2;}
    CGColorSpaceRef cs=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGFloat white[4]={1,1,1,1},blue[4]={.15,.35,.8,1};
    CGColorRef background=CGColorCreate(cs,white),foreground=CGColorCreate(cs,blue);
    [CATransaction begin];[CATransaction setDisableActions:YES];
    CALayer *root=[CALayer layer];root.frame=CGRectMake(0,0,W,H);root.backgroundColor=background;root.contentsScale=1;
    CALayer *capsule=[CALayer layer];capsule.frame=CGRectMake(64,64,384,128);capsule.backgroundColor=foreground;
    capsule.cornerRadius=64;capsule.masksToBounds=YES;capsule.contentsScale=1;capsule.allowsEdgeAntialiasing=YES;
    [root addSublayer:capsule];[CATransaction commit];[CATransaction flush];
    CARenderer *renderer=[CARenderer rendererWithMTLTexture:image options:@{kCARendererMetalCommandQueue:queue,kCARendererColorSpace:(__bridge id)cs}];
    if(!renderer){fprintf(stderr,"CARenderer creation returned nil.\n");return 2;}
    renderer.bounds=CGRectMake(0,0,W,H);renderer.layer=root;
    [CATransaction flush];
    [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];[renderer addUpdateRect:renderer.bounds];[renderer render];[renderer endFrame];
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
    [blit copyFromTexture:image sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:Pitch destinationBytesPerImage:Pitch*H];
    [blit endEncoding];[cb commit];[cb waitUntilCompleted];
    if(cb.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"Readback: %s\n",cb.error.localizedDescription.UTF8String);return 2;}
    const uint8_t *b=pixels.contents;NSUInteger insideBad=0,outsideBad=0,insideN=0,outsideN=0,colored=0;
    for(unsigned y=0;y<H;y++)for(unsigned x=0;x<W;x++){
        const uint8_t *v=b+y*Pitch+x*4;BOOL isBlue=v[2]<200 && v[0]>v[2]+20;if(isBlue)colored++;
        if(x<64||x>=448||y<64||y>=192)continue;
        double xx=x+.5,yy=y+.5,cx=xx<128 ? 128 : xx>384 ? 384 : xx;
        double dist=hypot(xx-cx,yy-128);
        if(dist<61){insideN++;if(!isBlue)insideBad++;}
        else if(dist>67){outsideN++;if(isBlue)outsideBad++;}
    }
    NSString *prefix=@(argv[1]);
    NSData *data=[NSData dataWithBytes:b length:Pitch*H];CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)data);
    CGImageRef cg=CGImageCreate(W,H,8,32,Pitch,cs,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst,provider,NULL,false,kCGRenderingIntentDefault);
    NSURL *url=[NSURL fileURLWithPath:[prefix stringByAppendingString:@".png"]];
    CGImageDestinationRef dest=CGImageDestinationCreateWithURL((__bridge CFURLRef)url,CFSTR("public.png"),1,NULL);
    BOOL saved=NO;if(dest){CGImageDestinationAddImage(dest,cg,NULL);saved=CGImageDestinationFinalize(dest);CFRelease(dest);}
    NSDictionary *report=@{@"device":dev.name,@"scope":@"Core Animation capsule rendered offscreen; no desktop/display configuration change",@"inside_checked":@(insideN),@"inside_wrong":@(insideBad),@"outside_checked":@(outsideN),@"outside_wrong":@(outsideBad),@"blue_pixels":@(colored),@"image_saved":@(saved),@"pass":@(insideBad==0&&outsideBad==0&&colored>0)};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];[json writeToFile:[prefix stringByAppendingString:@".json"] atomically:YES];fwrite(json.bytes,1,json.length,stdout);putchar('\n');
    if(cg)CGImageRelease(cg);CGDataProviderRelease(provider);CGColorRelease(background);CGColorRelease(foreground);CGColorSpaceRelease(cs);
    return [report[@"pass"] boolValue] ? 0 : 1;
} }
