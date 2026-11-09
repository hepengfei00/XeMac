#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#include <dlfcn.h>

// Reproduce texture input/output aliasing without registering any display.
int main(void) { @autoreleasepool {
    id<MTLDevice> device=nil;
    const char *plugin=getenv("NM_TEST_PLUGIN");
    if (plugin) {
        if (!dlopen(plugin,RTLD_NOW|RTLD_LOCAL)) { fprintf(stderr,"%s\n",dlerror()); return 2; }
        io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NMXAccel"));
        if (service) {
            device=((id (*)(id,SEL,mach_port_t))objc_msgSend)([NSClassFromString(@"NVMTLDevice") alloc],NSSelectorFromString(@"initWithAcceleratorPort:"),service);
            IOObjectRelease(service);
        }
    } else device=MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr,"No requested Metal device.\n"); return 2; }
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
    "vertex float4 v(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i],0,1);}\n"
    "fragment float4 fill() {return float4(.2,.4,.6,1);}\n"
    "fragment float4 readback(float4 p [[position]], texture2d<float,access::read> t [[texture(0)]]) {return float4(t.read(uint2(p.xy)).rgb+float3(.1),1);}\n";
    NSError *error=nil;
    id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
    NSMutableArray *pipelines=[NSMutableArray new];
    for (NSString *name in @[@"fill",@"readback"]) {
        MTLRenderPipelineDescriptor *pd=[MTLRenderPipelineDescriptor new];
        pd.vertexFunction=[library newFunctionWithName:@"v"];
        pd.fragmentFunction=[library newFunctionWithName:name];
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline=library ? [device newRenderPipelineStateWithDescriptor:pd error:&error] : nil;
        if (!pipeline) { fprintf(stderr,"Pipeline: %s\n",error.localizedDescription.UTF8String); return 2; }
        [pipelines addObject:pipeline];
    }
    enum { W=64,H=64,Pitch=W*4 };
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
    id<MTLTexture> input=[device newTextureWithDescriptor:td], output=[device newTextureWithDescriptor:td];
    id<MTLBuffer> pixels=[device newBufferWithLength:Pitch*H options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[device newCommandQueue];
    if (!input || !output || !pixels || !queue) { fprintf(stderr,"Resource creation failed.\n"); return 2; }
    NSArray *names=@[@"separate input texture",@"same attachment after render-target barrier",@"same attachment across render passes"];
    NSMutableArray *results=[NSMutableArray new];BOOL allGood=YES;
    for (NSUInteger test=0;test<names.count;test++) {
        id<MTLCommandBuffer> cb=[queue commandBuffer];
        id<MTLTexture> base=test==0 ? input : output;
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture=base;rp.colorAttachments[0].loadAction=MTLLoadActionClear;
        rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
        id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:pipelines[0]];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (test!=1) {
            [enc endEncoding];
            rp=[MTLRenderPassDescriptor renderPassDescriptor];rp.colorAttachments[0].texture=output;
            rp.colorAttachments[0].loadAction=test==0 ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction=MTLStoreActionStore;
            enc=[cb renderCommandEncoderWithDescriptor:rp];
        } else [enc memoryBarrierWithScope:MTLBarrierScopeRenderTargets afterStages:MTLRenderStageFragment beforeStages:MTLRenderStageFragment];
        [enc setRenderPipelineState:pipelines[1]];
        [enc setFragmentTexture:base atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[enc endEncoding];
        id<MTLBlitCommandEncoder> blit=[cb blitCommandEncoder];
        [blit copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:pixels destinationOffset:0 destinationBytesPerRow:Pitch destinationBytesPerImage:Pitch*H];
        [blit endEncoding];[cb commit];[cb waitUntilCompleted];
        if (cb.status!=MTLCommandBufferStatusCompleted) {
            allGood=NO;[results addObject:@{@"name":names[test],@"pass":@NO,@"error":cb.error.localizedDescription ?: @"Command incomplete"}];break;
        }
        const uint8_t *bytes=pixels.contents;unsigned wrong=0,maxError=0;unsigned expected[4]={179,128,77,255};
        NSMutableArray *examples=[NSMutableArray new];
        for (unsigned i=0;i<W*H;i++) {
            BOOL bad=NO;
            for (unsigned c=0;c<4;c++) { unsigned delta=abs((int)bytes[4*i+c]-(int)expected[c]);if(delta>maxError)maxError=delta;if(delta>2)bad=YES; }
            if (bad) { wrong++;if(examples.count<3)[examples addObject:@[@(i%W),@(i/W),@(bytes[4*i]),@(bytes[4*i+1]),@(bytes[4*i+2]),@(bytes[4*i+3])]]; }
        }
        if(wrong)allGood=NO;
        [results addObject:@{@"name":names[test],@"pass":@(wrong==0),@"wrong_pixels":@(wrong),@"pixels":@(W*H),@"maximum_channel_error":@(maxError),@"tolerance":@2,@"examples_xy_BGRA":examples}];
    }
    NSDictionary *report=@{@"device":device.name,@"scope":@"offscreen texture aliasing only; does not prove whole desktop or DP scanout",@"results":results,@"pass":@(allGood)};
    NSData *json=[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
    fwrite(json.bytes,1,json.length,stdout);putchar('\n');return allGood ? 0 : 1;
} }
