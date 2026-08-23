#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>

#define IAN_IOC_GET_RANDOM _IOR('i', 1, unsigned int)
#define IAN_IOC_SET_SEED   _IOW('i', 2, unsigned int)
#define IAN_IOC_GET_RATE   _IOR('i', 3, unsigned int)

int main()
{
	int fd = open("/dev/ian", O_RDWR);
	unsigned int val;
	
	ioctl(fd, IAN_IOC_GET_RANDOM, &val);
	printf("random = 0x%08x\n",val);
	
	ioctl(fd, IAN_IOC_GET_RATE, &val);
	printf("rate = %u Hz\n",val);
	
	val = 0x12345678;
	ioctl(fd, IAN_IOC_SET_SEED, &val);
	printf("seed set\n");
	return 0;
}