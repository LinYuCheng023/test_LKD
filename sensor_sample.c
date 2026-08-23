#if 0
// SPDX-License-Identifier: GPL-2.0
/*
 * hrtimer_jitter.c — 3.2：timer_list vs hr
 *
 * 同樣「每 period_ms 觸發」，比較實際間隔的抖動：
 *   timer_list：卡 HZ=100 tick 精度(10ms)，抖動大、無法 <10ms
 *   hrtimer   ：clockevent 硬體(arm_global_timer)，奈秒級，抖動小
 *
 * 用 ktime_get() 打時間戳，統計 jitter = |實際間隔 - 目標間隔|
 * 觀察彩蛋：timer_list callback in_interrupt=256(softirq)；hrtimer=65536(hardirq)
 *
 * 用法：
 *   insmod hrtimer_jitter.ko use_hrtimer=0   # timer_list 版
 *   insmod hrtimer_jitter.ko use_hrtimer=1   # hrtimer 版
 *   insmod hrtimer_jitter.ko use_hrtimer=1 period_ms=5  # 試 5ms（timer_list 做不到）
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/hrtimer.h>     /* ★ hrtimer */
#include <linux/ktime.h>
#include <linux/hardirq.h>
#include <linux/math64.h>

static int use_hrtimer = 0;
module_param(use_hrtimer, int, 0644);
static int period_ms = 10;
module_param(period_ms, int, 0644);
static int max_fires = 100;    /* 跑幾次就停，免得洗版 */
module_param(max_fires, int, 0644);

struct jit_dev {
      struct timer_list  tl;
      struct hrtimer     hrt;
      ktime_t            last;         /* 上次觸發時刻 */
      u64                expected_ns;  /* 目標間隔（奈秒）*/
      unsigned int       count;
      s64                max_jit_ns;   /* 最大抖動 */
      s64                sum_jit_ns;   /* 抖動總和（算平均）*/
};
static struct jit_dev jd;

/* 共用的「記錄一次觸發、算抖動」邏輯 */
static void record_fire(struct jit_dev *d, const char *who)
{
      ktime_t now = ktime_get();
      s64 delta, jit;

      if (d->count > 0) {                     /* 第一次沒有「上次」，跳過 */
              delta = ktime_to_ns(ktime_sub(now, d->last));   /* 實際間隔 */
              jit = delta - (s64)d->expected_ns;              /* 抖動（可正可負）*/
              if (jit < 0) jit = -jit;                        /* 取絕對值 */
              if (jit > d->max_jit_ns) d->max_jit_ns = jit;
              d->sum_jit_ns += jit;

              /* 每 20 次印一次，含這次實際間隔 + 上下文 */
              if (d->count % 20 == 0)
                      pr_info("%s: #%u 實際間隔=%lld us, 抖動=%lld us, in_interrupt=%ld\n",
                              who, d->count,
                              div_s64(delta , 1000), div_s64(jit , 1000), in_interrupt());
      }
      d->last = now;
      d->count++;
}

/* ── timer_list callback（softirq 上下文）── */
static void tl_fn(struct timer_list *t)
{
      struct jit_dev *d = from_timer(d, t, tl);

      record_fire(d, "timer_list");

      /* TODO 1：週期性 — 重新排 timer（到 max_fires 就停）
       *   if (d->count < max_fires)
       *       mod_timer(&d->tl, jiffies + msecs_to_jiffies(period_ms));
       */
	   if(d->count < max_fires)
		   mod_timer(&d->tl, jiffies + msecs_to_jiffies(period_ms));
}

/* ── hrtimer callback（預設 hardirq 上下文）── */
static enum hrtimer_restart hrt_fn(struct hrtimer *t)
{
      struct jit_dev *d = container_of(t, struct jit_dev, hrt);

      record_fire(d, "hrtimer   ");

      if (d->count >= max_fires)
              return HRTIMER_NORESTART;           /* 停 */

      /* TODO 2：週期性 — 把下次到期往後推 period_ms，並要求重啟
       *   hrtimer_forward_now(t, ms_to_ktime(period_ms));
       *   return HRTIMER_RESTART;
       */
	   
	   hrtimer_forward_now(t, ms_to_ktime(period_ms));
	   return HRTIMER_RESTART;
      return HRTIMER_NORESTART;   /* ← 填完 TODO 2 後改成上面的 RESTART */
}

static int __init jit_init(void)
{
      jd.expected_ns = (u64)period_ms * 1000000ULL;   /* ms → ns */

      if (use_hrtimer) {
              /* TODO 3：初始化並啟動 hrtimer
               *   hrtimer_init(&jd.hrt, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
               *   jd.hrt.function = hrt_fn;
               *   hrtimer_start(&jd.hrt, ms_to_ktime(period_ms), HRTIMER_MODE_REL);
               */
			   hrtimer_init(&jd.hrt, hrt_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
			   hrtimer_start(&jd.hrt, ms_to_ktime(period_ms), HRTIMER_MODE_REL);
              pr_info("jitter: hrtimer 版啟動，period=%dms\n", period_ms);
      } else {
              /* TODO 4：初始化並啟動 timer_list
               *   timer_setup(&jd.tl, tl_fn, 0);
               *    mod_timer(&jd.tl, jiffies + msecs_to_jiffies(period_ms));
               */
			   timer_setup(&jd.tl, tl_fn, 0);
			   mod_timer(&jd.tl, jiffies + msecs_to_jiffies(period_ms));
              pr_info("jitter: timer_list 版啟動，period=%dms（受 HZ=%d 限制）\n",
                      period_ms, HZ);
      }
      return 0;
}

static void __exit jit_exit(void)
{
      if (use_hrtimer)
              hrtimer_cancel(&jd.hrt);       /* 等 callback 跑完再取消 */
      else
              del_timer_sync(&jd.tl);

      if (jd.count > 1)
              pr_info("jitter: 結束 fires=%u, 最大抖動=%lld us, 平均抖動=%lld us\n",
                      jd.count, div_s64(jd.max_jit_ns , 1000),
                      div_s64(div_s64(jd.sum_jit_ns , (jd.count - 1)) , 1000));
}

module_init(jit_init);
module_exit(jit_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("3.2: timer_list vs hrtimer jitter");
#else
// SPDX-License-Identifier: GPL-2.0
/*
 * sensor_sample.c - 传感器采样框架大作业 (Linux 4.19/5.4)
 * 覆盖: CH7 + CH8 + CH10 + CH11 + CH13(字符设备)
 *
 * 数据流:
 *   hrtimer(采样节拍,硬中断,不能睡)
 *     -> schedule_work
 *       -> workqueue(能睡) 读"传感器" -> 写 kfifo -> 唤醒 read
 *         -> 用户 read() 从 kfifo 取数据(没数据就睡等)
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hrtimer.h>          // CH11: hrtimer
#include <linux/ktime.h>
#include <linux/workqueue.h>        // CH8: workqueue
#include <linux/kfifo.h>            // 缓冲
#include <linux/spinlock.h>         // CH10: 自旋锁
#include <linux/atomic.h>           // CH10: atomic
#include <linux/wait.h>             // CH11: 等待队列
#include <linux/fs.h>               // CH13: 字符设备
#include <linux/cdev.h>
#include <linux/uaccess.h>          // copy_to_user
#include <linux/slab.h>
#include <linux/delay.h>            // CH11: usleep_range

/* ===================== 参数 ===================== */
#define SAMPLE_PERIOD_MS    100         /* 采样周期: 100ms (10Hz) */
#define FIFO_SIZE           64          /* kfifo 容量(必须 2 的幂) */
#define DEV_NAME            "sensor_sample"

/* 一个采样数据 */
struct sample_data {
    u32 seq;            /* 序号 */
    u32 value;          /* 模拟传感器读数 */
    u64 timestamp_ns;   /* 采样时间戳 */
};

/* ===================== 设备结构 ===================== */
struct sensor_dev {
    /* CH11: 采样节拍 */
    struct hrtimer      sample_timer;
    ktime_t             period;

    /* CH8: 下半部(读传感器要睡) */
    struct work_struct  sample_work;

    /* 缓冲 + 保护 */
    struct kfifo        fifo;
    spinlock_t          fifo_lock;      /* CH10: 保护 kfifo */

    /* CH11: read 阻塞用的等待队列 */
    wait_queue_head_t   read_wq;

    /* CH10: 统计(多上下文访问,用 atomic) */
    atomic_t            sample_count;   /* 总采样次数 */
    atomic_t            drop_count;     /* fifo 满丢弃次数 */
    u32                 seq;            /* 序号(只在 work 里改,不用锁) */

    /* CH13: 字符设备 */
    dev_t               devt;
    struct cdev         cdev;
    struct class       *class;
    struct device      *device;

    bool                running;
};

static struct sensor_dev *g_dev;

/* ===================== 模拟读传感器 =====================
 * 真实场景这里是 i2c/spi 读硬件, 会睡
 */
static u32 fake_read_sensor(void)
{
    /* CH11: 这里在 workqueue(进程上下文), 可以睡!
     * 模拟 i2c 传输耗时
     */
    usleep_range(1000, 2000);   /* 假装读传感器花了 1~2ms */

    /* 返回一个模拟值(用 jiffies 制造变化) */
    return (u32)(jiffies & 0xFFFF);
}

/* ===================== 下半部: workqueue =====================
 * CH8: 进程上下文,能睡
 * 干真正的重活: 读传感器 + 写 fifo + 唤醒 read
 */
static void sample_work_fn(struct work_struct *w)
{
    struct sensor_dev *dev = container_of(w, struct sensor_dev, sample_work);
    struct sample_data data;
    unsigned long flags;
    int ret;

    /* 1. 读传感器(会睡, 在 workqueue 里 OK) */
    data.value = fake_read_sensor();
    data.seq = dev->seq++;
    data.timestamp_ns = ktime_get_ns();     /* CH11: 高精度时间戳(clocksource) */

    atomic_inc(&dev->sample_count);          /* CH10: 原子计数 */

    /* 2. 写入 kfifo (CH10: spin_lock 保护)
     * 注意: read() 也会碰 fifo, 但 read 在进程上下文,
     *       work 也在进程上下文 -> 用普通 spin_lock 即可
     *       (若有中断上下文碰 fifo 才需要 irqsave)
     */
    spin_lock_irqsave(&dev->fifo_lock, flags);
    ret = kfifo_in(&dev->fifo, &data, 1);   /* 放一个 sample_data */
    spin_unlock_irqrestore(&dev->fifo_lock, flags);

    if (ret == 0) {
        /* fifo 满, 丢弃 */
        atomic_inc(&dev->drop_count);
        pr_warn_ratelimited("%s: fifo full, drop seq=%u\n", DEV_NAME, data.seq);
    } else {
        /* 3. CH11: 唤醒等待数据的 read() */
        wake_up_interruptible(&dev->read_wq);
    }
}

/* ===================== hrtimer 采样回调 =====================
 * CH11 + CH8: 硬中断上下文! 绝对不能睡!
 * 只做: 触发下半部 + 推进到下个周期
 * (★这里绝不能 msleep, 否则就是你刚才那个 crash!)
 */
static enum hrtimer_restart sample_timer_cb(struct hrtimer *t)
{
    struct sensor_dev *dev = container_of(t, struct sensor_dev, sample_timer);

    if (!dev->running)
        return HRTIMER_NORESTART;

    /* CH8: 硬中断上下文, 要睡的活丢 workqueue */
    schedule_work(&dev->sample_work);

    /* CH11: 精确推进到下个周期(不累积误差) */
    hrtimer_forward_now(t, dev->period);
    return HRTIMER_RESTART;                  /* 周期性重启 */
}

/* ===================== 字符设备 read =====================
 * CH13: 用户空间 read() 进这里
 * CH11: 没数据就睡在等待队列, 有数据被唤醒
 */
static ssize_t sensor_read(struct file *filp, char __user *buf,
                           size_t count, loff_t *ppos)
{
    struct sensor_dev *dev = filp->private_data;
    struct sample_data data;
    unsigned long flags;
    int ret;

    if (count < sizeof(data))
        return -EINVAL;

    /* CH11: 等待队列 - fifo 空就睡, 直到有数据(或被信号打断)
     * 非阻塞模式(O_NONBLOCK)则立即返回
     */
    if (kfifo_is_empty(&dev->fifo)) {
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;

        /* 睡等: 直到 fifo 非空. 这里是进程上下文, 能睡 */
        ret = wait_event_interruptible(dev->read_wq,
                                       !kfifo_is_empty(&dev->fifo));
        if (ret)
            return ret;     /* 被信号打断 -ERESTARTSYS */
    }

    /* CH10: 取数据要加锁 */
    spin_lock_irqsave(&dev->fifo_lock, flags);
    ret = kfifo_out(&dev->fifo, &data, 1);
    spin_unlock_irqrestore(&dev->fifo_lock, flags);

    if (ret == 0)
        return 0;   /* 竞争: 别人抢先取走了, 返回 0 */

    /* CH13: 拷到用户空间 */
    if (copy_to_user(buf, &data, sizeof(data)))
        return -EFAULT;

    return sizeof(data);
}

static int sensor_open(struct inode *inode, struct file *filp)
{
    struct sensor_dev *dev = container_of(inode->i_cdev,
                                          struct sensor_dev, cdev);
    filp->private_data = dev;
    return 0;
}

static int sensor_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations sensor_fops = {
    .owner   = THIS_MODULE,
    .open    = sensor_open,
    .release = sensor_release,
    .read    = sensor_read,
    /* 进阶: 可加 .poll 支持 select/poll */
};

/* ===================== 初始化 ===================== */
static int __init sensor_init(void)
{
    struct sensor_dev *dev;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev)
        return -ENOMEM;

    /* --- 初始化各组件 --- */
    spin_lock_init(&dev->fifo_lock);            /* CH10 */
    init_waitqueue_head(&dev->read_wq);         /* CH11 */
    atomic_set(&dev->sample_count, 0);          /* CH10 */
    atomic_set(&dev->drop_count, 0);
    INIT_WORK(&dev->sample_work, sample_work_fn);/* CH8 */

    /* CH11: kfifo (存 sample_data, FIFO_SIZE 个) */
    ret = kfifo_alloc(&dev->fifo,
                      FIFO_SIZE * sizeof(struct sample_data),
                      GFP_KERNEL);
    if (ret) {
        pr_err("%s: kfifo_alloc failed\n", DEV_NAME);
        goto err_free;
    }

    /* CH13: 注册字符设备 */
    ret = alloc_chrdev_region(&dev->devt, 0, 1, DEV_NAME);
    if (ret)
        goto err_fifo;

    cdev_init(&dev->cdev, &sensor_fops);
    dev->cdev.owner = THIS_MODULE;
    ret = cdev_add(&dev->cdev, dev->devt, 1);
    if (ret)
        goto err_region;

    dev->class = class_create(DEV_NAME);   /* 5.10：只收 name */
    if (IS_ERR(dev->class)) {
        ret = PTR_ERR(dev->class);
        goto err_cdev;
    }

    dev->device = device_create(dev->class, NULL, dev->devt, NULL, DEV_NAME);
    if (IS_ERR(dev->device)) {
        ret = PTR_ERR(dev->device);
        goto err_class;
    }

    /* CH11: 启动 hrtimer 采样 */
    dev->period = ms_to_ktime(SAMPLE_PERIOD_MS);
    hrtimer_setup(&dev->sample_timer, sample_timer_cb, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    dev->running = true;
    hrtimer_start(&dev->sample_timer, dev->period, HRTIMER_MODE_REL);

    g_dev = dev;
    pr_info("%s: loaded. /dev/%s ready, sampling @ %dms\n",
            DEV_NAME, DEV_NAME, SAMPLE_PERIOD_MS);
    return 0;

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

/* ===================== 卸载 ===================== */
static void __exit sensor_exit(void)
{
    struct sensor_dev *dev = g_dev;

    /* ★清理顺序很重要! */

    /* 1. 停 hrtimer (先停源头, 之后不再产生新 work) */
    dev->running = false;
    hrtimer_cancel(&dev->sample_timer);         /* CH11: 等回调跑完 */

    /* 2. 等所有已排队的 work 跑完 (CH8) */
    cancel_work_sync(&dev->sample_work);

    /* 3. 唤醒可能还在 read 里睡的进程(让它们醒来退出) */
    wake_up_interruptible(&dev->read_wq);

    /* 4. 拆字符设备 */
    device_destroy(dev->class, dev->devt);
    class_destroy(dev->class);
    cdev_del(&dev->cdev);
    unregister_chrdev_region(dev->devt, 1);

    /* 5. 释放 fifo */
    kfifo_free(&dev->fifo);

    pr_info("%s: unloaded. total samples=%d, drops=%d\n",
            DEV_NAME,
            atomic_read(&dev->sample_count),
            atomic_read(&dev->drop_count));

    kfree(dev);
}

module_init(sensor_init);
module_exit(sensor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("your name");
MODULE_DESCRIPTION("Sensor sampling framework: hrtimer+workqueue+kfifo+waitqueue+cdev");
#endif