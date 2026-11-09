#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "nvrm-agdc-k5.h"
int main(void) {
 io_service_t fb=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching("XeGfxIntelKMSFramebuffer"));
 if(!fb)return 2;
 CFTypeRef existing=IORegistryEntryCreateCFProperty(fb,CFSTR("NVRMTiming"),kCFAllocatorDefault,0);
 printf("live NVRMTiming: %s\n",existing?"present":"ABSENT");
 if(existing){
  if(CFGetTypeID(existing)!=CFDataGetTypeID()||CFDataGetLength(existing)!=52)return 7;
  uint32_t live[13];memcpy(live,CFDataGetBytePtr(existing),sizeof live);
  if(nvk5_timing_bad(live,sizeof live))return 8;
  printf("published timing validated: %ux%u clock %u Hz\n",live[2],live[6],live[1]);
 }
 CFTypeRef arr=IORegistryEntryCreateCFProperty(fb,CFSTR("IOFBDetailedTimings"),kCFAllocatorDefault,0);
 if(!arr||CFGetTypeID(arr)!=CFArrayGetTypeID()||!CFArrayGetCount(arr))return 3;
 CFDataRef data=(CFDataRef)CFArrayGetValueAtIndex(arr,0);
 if(CFGetTypeID(data)!=CFDataGetTypeID()||CFDataGetLength(data)!=sizeof(IODetailedTimingInformationV2))return 4;
 IODetailedTimingInformationV2 d;memcpy(&d,CFDataGetBytePtr(data),sizeof d);
 uint32_t t[13]={NVK5_TIMING_TAG,(uint32_t)d.pixelClock,d.horizontalActive,d.horizontalActive+d.horizontalSyncOffset,d.horizontalActive+d.horizontalSyncOffset+d.horizontalSyncPulseWidth,d.horizontalActive+d.horizontalBlanking,d.verticalActive,d.verticalActive+d.verticalSyncOffset,d.verticalActive+d.verticalSyncOffset+d.verticalSyncPulseWidth,d.verticalActive+d.verticalBlanking,0,(d.horizontalSyncConfig&kIOSyncPositivePolarity)!=0,(d.verticalSyncConfig&kIOSyncPositivePolarity)!=0};
 t[10]=(uint32_t)(((uint64_t)t[1]*1000+(uint64_t)t[5]*t[9]/2)/((uint64_t)t[5]*t[9]));
 printf("measured framebuffer timing: %ux%u, clock %u Hz, total %ux%u, refresh %u mHz\n",t[2],t[6],t[1],t[5],t[9],t[10]);
 assert(nvk5_timing_bad(t,sizeof t)==0);
 struct NVAGDCLinkConfig req={0},out={0};int why=0;
 int r=nvk5_answer(NVK5_CMD_LINKCONFIG,&req,sizeof req,&out,sizeof out,NULL,0,&why);
 printf("without published timing: reply=%d why=0x%x\n",r,why);assert(r==NVK5_R_NOTFOUND&&why==0x31);
 why=0;r=nvk5_answer(NVK5_CMD_LINKCONFIG,&req,sizeof req,&out,sizeof out,t,sizeof t,&why);
 assert(r==NVK5_R_LINK&&out.timing.hActive==t[2]&&out.timing.vActive==t[6]&&out.timing.pixelClock==t[1]);
 printf("with actual timing: link reply OK, port %u, clock %llu Hz\n",out.port,out.timing.pixelClock);
 struct NVAGDCFBExtCap c={0},co={0};c.type=NVK5_CAP_PLANESCALER;
 r=nvk5_answer(NVK5_CMD_FBCAPEX,&c,sizeof c,&co,sizeof co,t,sizeof t,&why);
 assert(r==NVK5_R_CAPS&&co.count==1);uint32_t fmt;memcpy(&fmt,co.entry[0],4);assert(fmt==0x10001);
 puts("plane scaler + BGRA capabilities: OK");
 for(unsigned i=0;i<3;i++){c.type=(uint32_t[]){1,2,4}[i];r=nvk5_answer(NVK5_CMD_FBCAPEX,&c,sizeof c,&co,sizeof co,t,sizeof t,&why);assert(r==NVK5_R_CAPS&&co.count==1);}
 puts("colour matrix and gamma capabilities: OK");
 uint32_t bad[13];memcpy(bad,t,sizeof bad);bad[4]=bad[3];r=nvk5_answer(NVK5_CMD_LINKCONFIG,&req,sizeof req,&out,sizeof out,bad,sizeof bad,&why);assert(r==NVK5_R_NOTFOUND&&why==4);
 r=nvk5_answer(NVK5_CMD_LINKCONFIG,&req,sizeof req,&out,sizeof out,t,sizeof t-4,&why);assert(r==NVK5_R_NOTFOUND&&why==1);
 puts("invalid timing and truncated data remain rejected: OK");
 if(existing)CFRelease(existing);CFRelease(arr);IOObjectRelease(fb);return 0;
}
