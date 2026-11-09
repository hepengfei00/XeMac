#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <stdio.h>
int main(void){@autoreleasepool{
 const char *plugin=getenv("NM_TEST_PLUGIN");
 if(!plugin||!dlopen(plugin,RTLD_NOW|RTLD_LOCAL)){printf("plugin: %s\n",dlerror());return 2;}
 io_service_t svc=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("NMXAccel"));
 if(!svc){puts("no accelerator");return 2;}
 id<MTLDevice> dev=((id(*)(id,SEL,mach_port_t))objc_msgSend)([NSClassFromString(@"XeMTLDevice") alloc],NSSelectorFromString(@"initWithAcceleratorPort:"),svc);
 if(!dev){puts("no device");return 2;}
 id<MTLCommandQueue> q=[dev newCommandQueue];
 for(NSUInteger k=0;k<2;k++){
  NSUInteger length=k?5*1024*1024:319488,offset=256,pitch=1408,w=172,h=226;
  id<MTLBuffer> buf=[dev newBufferWithLength:length options:MTLResourceStorageModeShared];
  if(!buf||!buf.contents){puts("no shared buffer");return 3;}
  memset(buf.contents,0,length);
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  id<MTLTexture> tex=[buf newTextureWithDescriptor:d offset:offset bytesPerRow:pitch];
  printf("%s buffer-backed RGBA16Float: %s\n",k?"dedicated":"pooled",tex?"created":"REFUSED");fflush(stdout);
  if(!tex)return 4;
  MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=tex;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;pass.colorAttachments[0].clearColor=MTLClearColorMake(0,1,0.5,1);
  id<MTLCommandBuffer> cb=[q commandBuffer];id<MTLRenderCommandEncoder> enc=[cb renderCommandEncoderWithDescriptor:pass];[enc endEncoding];[cb commit];[cb waitUntilCompleted];
  if(cb.status!=MTLCommandBufferStatusCompleted){printf("GPU status %ld: %s\n",(long)cb.status,cb.error.localizedDescription.UTF8String);return 5;}
  NSUInteger xy[3][2]={{0,0},{85,112},{171,225}};
  for(int i=0;i<3;i++){uint16_t *v=(uint16_t*)((uint8_t*)buf.contents+offset+xy[i][1]*pitch+xy[i][0]*8);if(v[0]!=0||v[1]!=0x3c00||v[2]!=0x3800||v[3]!=0x3c00){printf("alias pixel mismatch %d: %04x %04x %04x %04x\n",i,v[0],v[1],v[2],v[3]);return 6;}}
  printf("%s alias GPU clear and CPU visibility: OK\n",k?"dedicated":"pooled");
 }
 return 0;
}}
