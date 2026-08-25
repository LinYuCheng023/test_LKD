// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_sample_todo.c — 傳感器採樣框架大作業（TODO 版）
 * 覆蓋 CH7/8 中斷下半部 + CH10 同步 + CH11 時間 + CH13 字元設備
 *
 * 資料流：
 *   hrtimer(採樣節拍, 中斷上下文, 不能睡)
 *     → schedule_work
 *       → workqueue(process, 能睡) 讀傳感器 → 寫 kfifo → 喚醒 read
 *         → 用戶 read() 從 kfifo 取(沒資料就睡等)
 *
 * ★ 已修正原骨架的 kfifo bug：改用「record kfifo」(DECLARE_KFIFO_PTR)，
 *   kfifo_in/out 的 count=1 才是「1 筆」，語義正確。
 *
 * 填完 6 個 TODO 即可跑：
 *   insmod sensor_sample_todo.ko
 *   cat /dev/sensor_sample | hexdump -C     # 或自己寫個讀 16 bytes 的程式
 *   rmmod sensor_sample_todo
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/wait.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/kref.h>
#include <linux/of.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include "sensor_ioctl.h"

#define SAMPLE_PERIOD_MS   100        /* 採樣週期 100ms（>板子 10ms 精度，安全）*/
#define FIFO_SIZE          64         /* kfifo 容量（筆數，必須 2 的冪）*/
#define DEV_NAME           "sensor_sample"
#define FAIL_TEST 		   1

static int fail_step = 0;
module_param(fail_step, int, 0644);

/* 一筆採樣資料 */
struct sample_data {
	u32 seq;            /* 序號 */
	u32 value;          /* 模擬讀數 */
	u64 timestamp_ns;   /* 採樣時間戳（單調時間）*/
};

struct sensor_dev {
	struct hrtimer      sample_timer;    /* CH11 採樣節拍 */
	ktime_t             period;
	u32                 period_ms;
	struct work_struct  sample_work;     /* CH8 下半部（讀傳感器要睡）*/

	/* ★ record kfifo：每個元素是一筆 sample_data（修正原 byte-kfifo bug）*/
	DECLARE_KFIFO_PTR(fifo, struct sample_data);
	spinlock_t          fifo_lock;       /* CH10 保護 kfifo（讀寫都 process context）*/
	spinlock_t          state_lock;      /* 保護 period/running */
	struct mutex	    control_lock;    /*讓 START/STOP 不會同時操作timer/work*/

	wait_queue_head_t   read_wq;         /* CH11 read 阻塞用 */
	bool                dying;           /* 卸載中：讓睡在 read 的進程醒來退出 */

	atomic_t            sample_count;    /* CH10 總採樣數 */
	atomic_t            drop_count;      /* CH10 fifo 滿丟棄數 */
	u32                 seq;             /* 序號（只在 work 改，不用鎖）*/

	dev_t               devt;            /* CH13 字元設備 */
	struct cdev         cdev;
	struct class       *class;
	struct device      *device;
	bool                running;
	struct kref 		ref;
};


/* 模擬讀傳感器（真實是 i2c/spi，會睡）*/
static u32 fake_read_sensor(void)
{
	usleep_range(1000, 2000);       /* 假裝 i2c 傳輸耗 1~2ms（在 workqueue 睡 OK）*/
	return (u32)(jiffies & 0xFFFF);
}

/* ── 下半部：workqueue（process context，能睡）── */
static void sample_work_fn(struct work_struct *w)
{
	struct sensor_dev *dev = container_of(w, struct sensor_dev, sample_work);
	struct sample_data data;
	unsigned long flags;
	int n;

	/* 1. 讀傳感器（會睡，在此 OK）*/
	data.value = fake_read_sensor();
	data.seq = dev->seq++;
	data.timestamp_ns = ktime_get_ns();     /* CH11 單調時間戳 */
	atomic_inc(&dev->sample_count);          /* CH10 原子計數 */

	/* 2. 寫入 kfifo（CH10 加鎖）
	 * TODO 1：拿鎖 → kfifo_in 一筆 → 放鎖
	 *   spin_lock_irqsave(&dev->fifo_lock, flags);
	 *   n = kfifo_in(&dev->fifo, &data, 1);   // record kfifo，1 = 一筆
	 *   spin_unlock_irqrestore(&dev->fifo_lock, flags);
	 */
	 spin_lock_irqsave(&dev->fifo_lock, flags);
	 n = kfifo_in(&dev->fifo, &data, 1);
	 spin_unlock_irqrestore(&dev->fifo_lock, flags);

	/* 3. 滿了就丟、否則喚醒 read
	 * TODO 2：
	 *   if (n == 0) {
	 *       atomic_inc(&dev->drop_count);
	 *       pr_warn_ratelimited("%s: fifo full, drop seq=%u\n", DEV_NAME, data.seq);
	 *   } else {
	 *       wake_up_interruptible(&dev->read_wq);   // CH11 喚醒等資料的 read
	 *   }
	 */
	 if(n == 0) {
		atomic_inc(&dev->drop_count);
		pr_warn_ratelimited("%s: fifo full, drop seq=%u\n", DEV_NAME, data.seq);
	 } else {
		 wake_up_interruptible(&dev->read_wq);
	 }
	 
}

/* ── hrtimer 採樣回調（中斷上下文，★絕不能睡★）── */
static enum hrtimer_restart sample_timer_cb(struct hrtimer *t)
{
	struct sensor_dev *dev = container_of(t, struct sensor_dev, sample_timer);
	ktime_t period;
	unsigned long flags;

	spin_lock_irqsave(&dev->state_lock, flags);
	if (!dev->running) {
		spin_unlock_irqrestore(&dev->state_lock, flags);
		return HRTIMER_NORESTART;
	}
	period = dev->period;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	/* TODO 3：把「要睡的重活」丟給下半部 + 週期性重排
	 *   ★ 這裡是中斷上下文，只能 schedule_work，絕不能 msleep/kmalloc(GFP_KERNEL)
	 *   schedule_work(&dev->sample_work);
	 *   hrtimer_forward_now(t, dev->period);   // 精確推進到下個週期
	 *   return HRTIMER_RESTART;
	 */
	 schedule_work(&dev->sample_work);
	hrtimer_forward_now(t, period);
	 
	return HRTIMER_RESTART;   /* ← 填完 TODO 3 後刪掉這行 */
}

static bool sensor_fifo_has_data(struct sensor_dev *dev)
{
	unsigned long flags;
	bool has_data;
	
	spin_lock_irqsave(&dev->fifo_lock, flags);
	has_data = !kfifo_is_empty(&dev->fifo);
	spin_unlock_irqrestore(&dev->fifo_lock, flags);
	
	return has_data;
}
/* ── 字元設備 read（process context，沒資料就睡）── */
static ssize_t sensor_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *ppos)
{
	struct sensor_dev *dev = filp->private_data;
	struct sample_data data;
	unsigned long flags;
	int n;

	if (count < sizeof(data))
		return -EINVAL;
	 
	 /*先嘗試取
	  → 取不到才 wait
	  → 醒來再嘗試取 這樣即使多個 reader 同時被喚醒，輸掉競爭的 reader 也會回到 kernel 內繼續
		等，不會把 read() == 0 暴露給 user-space。*/
	 while (1) {
		
		spin_lock_irqsave(&dev->fifo_lock, flags);
		n = kfifo_out(&dev->fifo, &data, 1);
		spin_unlock_irqrestore(&dev->fifo_lock, flags);
		 
		if(n == 1)
			 break;
		 
		if(filp->f_flags & O_NONBLOCK)
			return -EAGAIN;
		
		if(wait_event_interruptible(dev->read_wq, sensor_fifo_has_data(dev) || READ_ONCE(dev->dying)))
			return -ERESTARTSYS;
		
		if(READ_ONCE(dev->dying))
			return -ENODEV;

	}

	/* CH13：拷到用戶空間 */
	if (copy_to_user(buf, &data, sizeof(data)))
		return -EFAULT;
	return sizeof(data);
}
static void sensor_dev_free(struct kref *ref)
{
	struct sensor_dev *dev = container_of(ref, struct sensor_dev, ref);
	
	kfifo_free(&dev->fifo);              
	kfree(dev);
}
static int sensor_open(struct inode *inode, struct file *filp)
{
	struct sensor_dev *dev = container_of(inode->i_cdev, struct sensor_dev, cdev);
	kref_get(&dev->ref);
	filp->private_data = dev;
	return 0;
}
static int sensor_release(struct inode *inode, struct file *filp) 
{ 
	struct sensor_dev *dev = filp->private_data;
	
	kref_put(&dev->ref, sensor_dev_free);
	return 0; 
}

static long sensor_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct sensor_dev *dev = filp->private_data;
	struct sensor_stats stats;
	u32 period_ms;
	ktime_t period;
	bool need_start = false;
	unsigned long flags;
	
	if(READ_ONCE(dev->dying))
		return -ENODEV;
	
	switch (cmd) {
		case SENSOR_IOC_SET_PERIOD:
			if (copy_from_user(&period_ms, (void __user *)arg, sizeof(period_ms)))
				return -EFAULT;
			
			if (period_ms < 10 || period_ms > 60000)
				return -EINVAL;
			
			spin_lock_irqsave(&dev->state_lock, flags);
			dev->period_ms = period_ms;
			dev->period = ms_to_ktime(period_ms);
			spin_unlock_irqrestore(&dev->state_lock, flags);
			return 0;
		
		case SENSOR_IOC_GET_PERIOD:
			spin_lock_irqsave(&dev->state_lock, flags);
			period_ms = dev->period_ms;
			spin_unlock_irqrestore(&dev->state_lock, flags);
			if (copy_to_user((void __user *)arg, &period_ms ,sizeof(period_ms)))
				return -EFAULT;
			return 0;
			
		case SENSOR_IOC_GET_STATS:
			memset(&stats, 0 ,sizeof(stats));
			stats.sample_count = atomic_read(&dev->sample_count);
			stats.drop_count = atomic_read(&dev->drop_count);
			spin_lock_irqsave(&dev->fifo_lock, flags);
			stats.fifo_len = kfifo_len(&dev->fifo);
			spin_unlock_irqrestore(&dev->fifo_lock, flags);
			
			spin_lock_irqsave(&dev->state_lock, flags);
			stats.period_ms = dev->period_ms;
			stats.running = dev->running;
			spin_unlock_irqrestore(&dev->state_lock, flags);
			
			if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
				return -EFAULT;
			return 0;
		
		case SENSOR_IOC_RESET_STATS:
			atomic_set(&dev->sample_count, 0);
			atomic_set(&dev->drop_count, 0);
			return 0;
		
		case SENSOR_IOC_START:
			mutex_lock(&dev->control_lock);
			
			if (READ_ONCE(dev->dying)) {
				mutex_unlock(&dev->control_lock);
				return -ENODEV;
			}
			
			spin_lock_irqsave(&dev->state_lock, flags);
			if (!dev->running) {
				dev->running = true;
				period = dev->period;
				need_start = true;
			}
			stats.running = dev->running;
			spin_unlock_irqrestore(&dev->state_lock, flags);

			if (need_start) {
				hrtimer_start(&dev->sample_timer, period, HRTIMER_MODE_REL);
			}

			mutex_unlock(&dev->control_lock);
			
			if (copy_to_user((void __user *)arg, &stats.running, sizeof(stats.running)))
				return -EFAULT;
			return 0;
		
		case SENSOR_IOC_STOP:
			mutex_lock(&dev->control_lock);
			spin_lock_irqsave(&dev->state_lock, flags);
			dev->running = false;
			stats.running = false;
			spin_unlock_irqrestore(&dev->state_lock, flags);
			hrtimer_cancel(&dev->sample_timer);
			cancel_work_sync(&dev->sample_work);
			mutex_unlock(&dev->control_lock);
			
			if (copy_to_user((void __user *)arg, &stats.running, sizeof(stats.running)))
				return -EFAULT;
			return 0;
		
		case SENSOR_IOC_FLUSH_FIFO:
			spin_lock_irqsave(&dev->fifo_lock, flags);
			kfifo_reset(&dev->fifo);
			spin_unlock_irqrestore(&dev->fifo_lock, flags);
			return 0;
			
		default:
			return -ENOTTY;
	}
}

static __poll_t sensor_poll(struct file *filp, poll_table *wait)
{
	struct sensor_dev *dev = filp->private_data;
	__poll_t mask = 0;
	unsigned long flags;
	
	poll_wait(filp, &dev->read_wq, wait);
	
	spin_lock_irqsave(&dev->fifo_lock, flags);
	if (!kfifo_is_empty(&dev->fifo))
		mask |= POLLIN | POLLRDNORM;
	spin_unlock_irqrestore(&dev->fifo_lock, flags);
	
	if (READ_ONCE(dev->dying))
		mask |= POLLHUP;
	
	return mask;
}

static ssize_t period_ms_show(struct device *device, 
							  struct device_attribute *attr, 
							  char *buf)
{
	struct sensor_dev *dev = dev_get_drvdata(device);
	unsigned long flags;
	u32 period_ms;

	spin_lock_irqsave(&dev->state_lock, flags);
	period_ms = dev->period_ms;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	return sysfs_emit(buf, "%u\n", period_ms);
}

static ssize_t stats_show(struct device * device,
						  struct device_attribute *attr,
						  char *buf)
{
	struct sensor_dev *dev = dev_get_drvdata(device);
	unsigned long flags;
	struct sensor_stats stats;	

	memset(&stats, 0 ,sizeof(stats));

	stats.sample_count = atomic_read(&dev->sample_count);
	stats.drop_count = atomic_read(&dev->drop_count);
	spin_lock_irqsave(&dev->fifo_lock, flags);
	stats.fifo_len = kfifo_len(&dev->fifo);
	spin_unlock_irqrestore(&dev->fifo_lock, flags);
	
	spin_lock_irqsave(&dev->state_lock, flags);
	stats.period_ms = dev->period_ms;
	stats.running = dev->running;
	spin_unlock_irqrestore(&dev->state_lock, flags);

	return sysfs_emit(buf, "sample_count=%u drop_count=%u fifo_len=%u period_ms=%u running=%u\n",
		stats.sample_count, stats.drop_count, stats.fifo_len, stats.period_ms, stats.running);
}

static ssize_t period_ms_store(struct device *device, 
	                           struct device_attribute *attr,
							   const char *buf,
							   size_t count)
{
	struct sensor_dev *dev = dev_get_drvdata(device);
	unsigned long flags;
	u32 period_ms;
	int ret;

	ret = kstrtou32(buf, 10, &period_ms);
	if (ret)
		return ret;

	if (period_ms < 10 || period_ms > 60000)
		return -EINVAL;

	spin_lock_irqsave(&dev->state_lock, flags);
	dev->period_ms = period_ms;
	dev->period = ms_to_ktime(period_ms);
	spin_unlock_irqrestore(&dev->state_lock, flags);

	return count;
}

static DEVICE_ATTR_RW(period_ms);
static DEVICE_ATTR_RO(stats);

static const struct file_operations sensor_fops = {
	.owner   = THIS_MODULE,
	.open    = sensor_open,
	.release = sensor_release,
	.read    = sensor_read,
	.unlocked_ioctl = sensor_ioctl,
	.poll    = sensor_poll,
};

static int sensor_probe(struct platform_device *pdev)
{
	pr_info("sensor_probe!!");
	struct sensor_dev *dev;
	int ret;
	u32 period_ms = SAMPLE_PERIOD_MS;
	
	/* 建立「這顆 device」的 private data */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	/* 初始化這顆 device 的 synchronization/state */
	spin_lock_init(&dev->fifo_lock);
	spin_lock_init(&dev->state_lock);
	mutex_init(&dev->control_lock);
	init_waitqueue_head(&dev->read_wq);
	kref_init(&dev->ref);
	
	atomic_set(&dev->sample_count, 0);
	atomic_set(&dev->drop_count, 0);
	
	/* 初始化這顆 device 的 asynchronous work */
	INIT_WORK(&dev->sample_work, sample_work_fn);

	/* 建立這顆 device 的 FIFO */
	ret = kfifo_alloc(&dev->fifo, FIFO_SIZE, GFP_KERNEL);

	if (ret) { pr_err("%s: kfifo_alloc failed\n", DEV_NAME); goto err_free; }
#if FAIL_TEST
	if (fail_step == 1) { ret = -EIO; goto err_fifo; }
#endif

	/* CH13 字元設備註冊 */
	ret = alloc_chrdev_region(&dev->devt, 0, 1, DEV_NAME);

	if (ret) goto err_fifo;
#if FAIL_TEST
	if (fail_step == 2) { ret = -EIO; goto err_region; }
#endif
	cdev_init(&dev->cdev, &sensor_fops);
	dev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&dev->cdev, dev->devt, 1);
	if (ret) goto err_region;
#if FAIL_TEST
	if (fail_step == 3) { ret = -EIO; goto err_region; }
#endif

	dev->class = class_create(DEV_NAME);   /* 5.10：只收 name */
	
	if (IS_ERR(dev->class)) { ret = PTR_ERR(dev->class); goto err_cdev; }
#if FAIL_TEST
	if (fail_step == 4) { ret = -EIO; goto err_class; }
#endif

	dev->device = device_create(dev->class, NULL, dev->devt, NULL, DEV_NAME);

	if (IS_ERR(dev->device)) { ret = PTR_ERR(dev->device); goto err_class; }
#if FAIL_TEST
	if (fail_step == 5) { ret = -EIO; goto err_dev; }
#endif

	dev_set_drvdata(dev->device, dev);
	ret = device_create_file(dev->device, &dev_attr_period_ms);
	if (ret) {
		pr_err("%s: device_create_file failed\n", DEV_NAME);
		goto err_dev;
	}
	
	ret = device_create_file(dev->device, &dev_attr_stats);
	if (ret) {
		pr_err("%s: device_create_file failed\n", DEV_NAME);
		goto err_dev_attr_period_ms;
	}
	
	if (device_property_present(&pdev->dev, "sample-period-ms")) {
		ret = device_property_read_u32(&pdev->dev, "sample-period-ms", &period_ms);

		if (ret) {
			dev_err(&pdev->dev, "invalid sample-period-ms: %d\n", ret);
			goto err_dev_attr_stats;
		}
	}
	
	if (period_ms < 10 || period_ms > 60000) {
		ret =  -EINVAL;
		goto err_dev_attr_stats;
	}

	/* 建立這顆 device 的 timer */
	dev->period_ms = period_ms;
	dev->period = ms_to_ktime(period_ms);
	hrtimer_setup(&dev->sample_timer, sample_timer_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dev->running = true;
	hrtimer_start(&dev->sample_timer, dev->period, HRTIMER_MODE_REL);
	
	/* pdev <-> dev 建立關係 */
	platform_set_drvdata(pdev, dev);

	pr_info("%s: loaded. /dev/%s ready, sampling @ %dms\n",
		DEV_NAME, DEV_NAME, period_ms);
	return 0;

err_dev_attr_stats:
	device_remove_file(dev->device, &dev_attr_stats);
err_dev_attr_period_ms:
	device_remove_file(dev->device, &dev_attr_period_ms);
err_dev:    
	device_destroy(dev->class, dev->devt);
err_class: 
	class_destroy(dev->class);
err_cdev:   
	cdev_del(&dev->cdev);
err_region:
	unregister_chrdev_region(dev->devt, 1);
err_fifo:   
	kfifo_free(&dev->fifo);
err_free:   
	kfree(dev);
	return ret;
}

static void sensor_remove(struct platform_device *pdev)
{
	pr_info("sensor_remove!!");
	struct sensor_dev *dev;
	
	dev = platform_get_drvdata(pdev);

	if (!dev)
        return;
	
	platform_set_drvdata(pdev, NULL);
	
	/* 清理順序（反向拆解）：*/
	mutex_lock(&dev->control_lock);
	
	WRITE_ONCE(dev->dying, true); 
	
	spin_lock(&dev->state_lock);
	dev->running = false;
	spin_unlock(&dev->state_lock);
	
	hrtimer_cancel(&dev->sample_timer);   /* 1. 停 timer（等回調跑完）*/
	cancel_work_sync(&dev->sample_work);  /* 2. 等已排隊的 work 跑完 */

	mutex_unlock(&dev->control_lock);
    
	wake_up_interruptible(&dev->read_wq);

	device_remove_file(dev->device, &dev_attr_stats);
	device_remove_file(dev->device, &dev_attr_period_ms);
	device_destroy(dev->class, dev->devt);/* 4. 拆字元設備 */
	class_destroy(dev->class);
	cdev_del(&dev->cdev);
	unregister_chrdev_region(dev->devt, 1);


	pr_info("%s: unloaded. samples=%d, drops=%d\n", DEV_NAME,
		atomic_read(&dev->sample_count), atomic_read(&dev->drop_count));
	
	kref_put(&dev->ref, sensor_dev_free);
}

static const struct of_device_id sensor_of_match[] = {
	{ .compatible = "ian,sensor-sample" },
	{ }
};

MODULE_DEVICE_TABLE(of, sensor_of_match);

static struct platform_driver sensor_driver = {
	.probe = sensor_probe,
	.remove = sensor_remove,
	
	.driver = {
		.name = DEV_NAME,
		.of_match_table = sensor_of_match,
	},
};

static int __init sensor_init(void)
{
	return platform_driver_register(&sensor_driver);
}

static void __exit sensor_exit(void)
{
	platform_driver_unregister(&sensor_driver);
}

module_init(sensor_init);
module_exit(sensor_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("Sensor sampling: hrtimer+workqueue+kfifo+waitqueue+cdev (TODO)");
