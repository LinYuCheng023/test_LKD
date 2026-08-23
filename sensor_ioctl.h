#ifndef SENSOR_IOCTL_H
#define SENSOR_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SENSOR_IOC_MAGIC       'S'

struct sensor_stats {
	__u32 sample_count;
	__u32 drop_count;
	__u32 fifo_len;
	__u32 period_ms;
	__u32 running;
};

#define SENSOR_IOC_SET_PERIOD  _IOW(SENSOR_IOC_MAGIC, 1, __u32)
#define SENSOR_IOC_GET_PERIOD  _IOR(SENSOR_IOC_MAGIC, 2, __u32)
#define SENSOR_IOC_GET_STATS   _IOR(SENSOR_IOC_MAGIC, 3, struct sensor_stats)
#define SENSOR_IOC_RESET_STATS _IO(SENSOR_IOC_MAGIC, 4)
#define SENSOR_IOC_START	   _IOR(SENSOR_IOC_MAGIC, 5, __u32)
#define SENSOR_IOC_STOP        _IOR(SENSOR_IOC_MAGIC, 6, __u32)
#define SENSOR_IOC_FLUSH_FIFO  _IO(SENSOR_IOC_MAGIC, 7)

#endif
