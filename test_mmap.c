#include <stdio.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main()
{
	int fd = open("/dev/ian", O_RDWR);
	unsigned int *buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	
	
	printf("buf[0] = 0x%08x\n",buf[0]);
	buf[0] = 0x12345678;
	printf("buf[0] = 0x%08x\n",buf[0]);
	
	munmap(buf, 4096);
	close(fd);
	return 0;
}