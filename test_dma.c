#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>


int main(void)
{
    int fd = open("/dev/ian", O_RDWR);
	volatile unsigned int *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	
	if(p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	
	printf("buf[0] = 0x%08x\n",p[0]);
	p[1] = 0xCAFEBABE;
	pause();
	
    return 0;
}