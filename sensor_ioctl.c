#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sensor_ioctl.h"

int main(int argc, char **argv)
{
	int fd;
	uint32_t period;
	uint32_t running;
	struct sensor_stats stats;

	fd = open("/dev/sensor_sample", O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "open: %s\n", strerror(errno));
		return 1;
	}

	if (ioctl(fd, SENSOR_IOC_GET_PERIOD, &period) < 0) {
		perror("GET_PERIOD");
		return 1;
	}
	printf("period = %u ms\n", period);

	if (argc == 2) {
		if (strcmp(argv[1], "reset") == 0) {
			if (ioctl(fd, SENSOR_IOC_RESET_STATS) < 0) {
				perror("SENSOR_IOC_RESET_STATS");
				return 1;
			}
			printf("stats reset\n");
			return 0;
		} else if (strcmp(argv[1], "start") == 0) {
			if (ioctl(fd, SENSOR_IOC_START, &running) < 0) {
				perror("SENSOR_IOC_START");
				return 1;
			}
			printf("stats start running = %u\n",running);
			return 0;
		} else if (strcmp(argv[1], "stop") == 0) {
			if (ioctl(fd, SENSOR_IOC_STOP, &running) < 0) {
				perror("SENSOR_IOC_STOP");
				return 1;
			}
			printf("stats stop running = %u\n",running);
			return 0;
		} else if (strcmp(argv[1], "flush") == 0) {
			if (ioctl(fd, SENSOR_IOC_FLUSH_FIFO) < 0) {
				perror("SENSOR_IOC_FLUSH_FIFO");
				return 1;
			}
			printf("stats flush\n");
			return 0;
		}
		
		
		period = (uint32_t)strtoul(argv[1], NULL, 10);
		if (ioctl(fd, SENSOR_IOC_SET_PERIOD, &period) < 0) {
			perror("SET_PERIOD");
			return 1;
		}
		printf("new period = %u ms\n", period);
	}

	if (ioctl(fd, SENSOR_IOC_GET_STATS, &stats) < 0) {
		perror("GET_STATS");
		return 1;
	}
	printf("samples=%u drops=%u fifo_len=%u period=%u ms\n",
	       stats.sample_count, stats.drop_count,
	       stats.fifo_len, stats.period_ms);

	close(fd);
	return 0;
}
