#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
int main(int argc,char**argv){if(argc!=2)return 2;off_t offset=(off_t)strtoull(argv[1],0,0);int fd=open("/usr/sbin/bluetoothd",O_RDONLY);if(fd<0)return 3;unsigned char direct[7];ssize_t n=pread(fd,direct,7,offset);printf("PREAD n=%zd ",n);for(int i=0;i<7;i++)printf("%02x",direct[i]);puts("");long page=sysconf(_SC_PAGESIZE);off_t base=offset/page*page;for(int x=0;x<2;x++){int prot=PROT_READ|(x?PROT_EXEC:0);void*map=mmap(0,(size_t)page,prot,MAP_PRIVATE,fd,base);if(map==MAP_FAILED){printf("MMAP_%s failed errno=%d %s\n",x?"RX":"R",errno,strerror(errno));continue;}volatile const unsigned char *bytes=(unsigned char*)map+(offset-base);printf("MMAP_%s ",x?"RX":"R");for(int i=0;i<7;i++)printf("%02x",bytes[i]);puts("");munmap(map,(size_t)page);}close(fd);return 0;}
