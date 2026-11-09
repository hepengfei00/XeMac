#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
int main(void) {
 io_registry_entry_t options=IORegistryEntryFromPath(kIOMainPortDefault,"IODeviceTree:/options");
 if(!options){puts("OPTIONS_UNAVAILABLE");return 2;}
 const char* names[]={"bluetoothInternalControllerInfo","bluetoothExternalDongleFailed","bluetoothHostControllerSwitchBehavior"};
 int valid=1;
 for(unsigned i=0;i<3;i++){
  CFStringRef key=CFStringCreateWithCString(0,names[i],kCFStringEncodingUTF8);
  CFTypeRef value=IORegistryEntryCreateCFProperty(options,key,0,0);CFRelease(key);
  if(!value){printf("%s=MISSING\n",names[i]);valid=0;continue;}
  if(CFGetTypeID(value)==CFDataGetTypeID()){
   CFIndex length=CFDataGetLength(value);const UInt8* data=CFDataGetBytePtr(value);
   int zero=1;for(CFIndex n=0;n<length;n++)if(data[n])zero=0;
   printf("%s=CFData LENGTH=%ld ALL_ZERO=%d\n",names[i],(long)length,zero);
   if(i==0&&(length!=14||!zero))valid=0;
   if(i==1&&(length!=1||!zero))valid=0;
  }else{printf("%s=OTHER_TYPE\n",names[i]);if(i<2)valid=0;}
  CFRelease(value);
 }
 IOObjectRelease(options);return valid?0:3;
}
