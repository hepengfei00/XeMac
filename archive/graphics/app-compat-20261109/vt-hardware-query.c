#include <VideoToolbox/VideoToolbox.h>
#include <stdio.h>
int main(void) {
 const struct { const char *name; CMVideoCodecType type; } codecs[] = {
  {"H264",'avc1'}, {"HEVC",'hvc1'}, {"VP9",'vp09'}, {"AV1",'av01'}
 };
 puts("QUERY=VTIsHardwareDecodeSupported (availability query, not a timed decode test)");
 for(unsigned i=0;i<sizeof(codecs)/sizeof(codecs[0]);i++)
  printf("%s_HARDWARE_DECODE_AVAILABLE=%d\n",codecs[i].name,(int)VTIsHardwareDecodeSupported(codecs[i].type));
 return 0;
}
