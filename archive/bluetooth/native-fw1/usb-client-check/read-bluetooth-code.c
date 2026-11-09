#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld_images.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
static kern_return_t rd(task_t task,mach_vm_address_t address,void*output,mach_vm_size_t size){mach_vm_size_t got=0;kern_return_t r=mach_vm_read_overwrite(task,address,size,(mach_vm_address_t)output,&got);return r?r:(got==size?KERN_SUCCESS:KERN_FAILURE);}
int main(int argc,char**argv){if(argc!=2)return 2;int pid=atoi(argv[1]);task_t task=MACH_PORT_NULL;kern_return_t r=task_for_pid(mach_task_self(),pid,&task);printf("PID=%d TASK_PORT_RESULT=%d (%s)\n",pid,r,mach_error_string(r));if(r)return 0;task_dyld_info_data_t info;mach_msg_type_number_t count=TASK_DYLD_INFO_COUNT;r=task_info(task,TASK_DYLD_INFO,(task_info_t)&info,&count);printf("DYLD_INFO_RESULT=%d\n",r);if(r)return 0;struct dyld_all_image_infos images;memset(&images,0,sizeof(images));r=rd(task,info.all_image_info_addr,&images,sizeof(images));printf("IMAGE_INFO_READ=%d count=%u\n",r,images.infoArrayCount);if(r)return 0;unsigned limit=images.infoArrayCount<128?images.infoArrayCount:128;for(unsigned i=0;i<limit;i++){struct dyld_image_info image;r=rd(task,(mach_vm_address_t)(uintptr_t)images.infoArray+i*sizeof(image),&image,sizeof(image));if(r)break;char path[512]={0};r=rd(task,(mach_vm_address_t)(uintptr_t)image.imageFilePath,path,sizeof(path)-1);if(r)continue;if(!strcmp(path,"/usr/sbin/bluetoothd")){unsigned char code[8]={0};mach_vm_address_t address=(mach_vm_address_t)(uintptr_t)image.imageLoadAddress+0xe9d18;r=rd(task,address,code,7);printf("LIVE_VENDOR_CHECK_READ=%d bytes=",r);for(int j=0;j<7;j++)printf("%02x",code[j]);puts("");break;}}mach_port_deallocate(mach_task_self(),task);return 0;}
