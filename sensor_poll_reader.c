#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct sample_data {
	uint32_t seq;
	uint32_t value;
	uint64_t timestamp_ns;
};

int main(void)
{
	int fd = open("/dev/sensor_sample", O_RDONLY | O_NONBLOCK);
	struct pollfd pfd[2] = {
		{ .fd = fd, .events = POLLIN },
		{ .fd = STDIN_FILENO, .events = POLLIN },
	};
	struct sample_data sample;
	char input[32];
	
	if (fd < 0) {
		perror("open sensor");
		return 1;
	}
	
	while (1) {
		//printf("等待資料...\n");
		
		int ret = poll(pfd, 2, 3000);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			break;
		}
		
		if (ret == 0) {
			 puts("timeout：3 秒內沒有資料");
             continue;
		}
		
		if (pfd[1].revents & POLLIN) {
			
			if (fgets(input, sizeof(input), stdin) && input[0] == 'q')
				break;
			printf("讀到 input = %s\n", input);
		}
		
		if (pfd[0].revents & POLLIN) {
			ret = read(fd, &sample, sizeof(sample));
			
			if (ret == sizeof(sample))
				printf("讀到 seq=%u value=0x%x\n", sample.seq, sample.value);
			else if (ret < 0 && errno != EAGAIN) {
				perror("read sensor");
				break;
			}
				
		}
		
		if (pfd[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
			puts("sensor fd 發生錯誤或關閉");
            break;
		}
		
	}
	close(fd);
	return 0;
}