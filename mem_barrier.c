// SPDX-License-Identifier: GPL-2.0
/*
 * mem_barrier.c — 3.1：内存屏障（双核才能看到 ARM 弱序乱序）
 *
 * 两个 kthread 各绑一颗核：
 *   producer(CPU0): data = 魔术值; [smp_wmb]; flag = 1;
 *   consumer(CPU1): while(flag==0); [smp_rmb]; 检查 data 对不对
 *
 * use_barrier=0 → 不加屏障 → ARM 弱序可能让 consumer 看到 flag=1 但 data 还旧 → 抓到乱序
 * use_barrier=1 → 加 smp_wmb/smp_rmb → 永远正确
 *
 * ★ 单核跑不出来（smp_* 退化成 barrier + 无真并行）；这台双核才有意义。
 * ★ 乱序是概率性的，要大量迭代才可能抓到，抓不到不代表没有（弱序 bug 的特性）。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/atomic.h>

static int use_barrier = 1;
module_param(use_barrier, int, 0644);
MODULE_PARM_DESC(use_barrier, "1=加 smp_wmb/smp_rmb（正确）；0=不加（可能抓到乱序）");

#define MAGIC 0x12345678

/* ★ 共享变量：两核一写一读。故意分开放，别让它们挤同一 cache line */
static int shared_data;
static int shared_flag;

static struct task_struct *producer_thread;
static struct task_struct *consumer_thread;

static atomic_t mismatch = ATOMIC_INIT(0);   /* consumer 抓到「flag=1 但 data 错」的次数 */
static atomic_t rounds   = ATOMIC_INIT(0);   /* 总回合数 */

static int g_seq;   /* 每回合递增的序号；data 和 flag 都写它 */

/* ── producer：写 data=seq、写 flag=seq（绑 CPU0）── */
static int producer_fn(void *arg)
{
      while (!kthread_should_stop()) {
              /* 握手：等上一回合 consumer 读完（flag 被清 0）*/
              while (READ_ONCE(shared_flag) != 0) {
                      if (kthread_should_stop()) return 0;
                      cpu_relax();
              }

              g_seq++;                              /* ★ 每回合不同的值 */
              WRITE_ONCE(shared_data, g_seq);       /* ① 写 data = seq */

              if (use_barrier)
                      smp_wmb();                    /* 写屏障：保证 data 先出去 */

              WRITE_ONCE(shared_flag, g_seq);       /* ② 写 flag = 同一个 seq */

              atomic_inc(&rounds);
              if (atomic_read(&rounds) % 100000 == 0)
                      pr_info("mb: rounds=%d, mismatch=%d (use_barrier=%d)\n",
                             atomic_read(&rounds), atomic_read(&mismatch), use_barrier);
      }
      return 0;
}

/* ── consumer：等 flag、读 data（绑 CPU1）── */
static int consumer_fn(void *arg)
{
      while (!kthread_should_stop()) {
              int f, d;

              /* 等 flag 变非 0（producer 写了 seq）*/
              while ((f = READ_ONCE(shared_flag)) == 0) {
                      if (kthread_should_stop()) return 0;
                      cpu_relax();
              }

              if (use_barrier)
                      smp_rmb();                    /* 读屏障：保证先读 flag 再读 data */

              d = READ_ONCE(shared_data);

              /* ★ data 和 flag 本该是同一个 seq。
               *   若 d != f（通常 d 比 f 旧/小）→ flag 到了但 data 还没 → 抓到乱序！*/
              if (d != f)
                      atomic_inc(&mismatch);

              /* 握手：读完清 flag，通知 producer 开下一回合 */
              WRITE_ONCE(shared_flag, 0);
      }
      return 0;
}

static int __init mb_init(void)
{
      pr_info("mb: loaded, use_barrier=%d（0=可能抓乱序, 1=正确）\n", use_barrier);

      /* TODO 3：建两个 kthread，各绑一颗核（关键：要在不同核才有真并行）
       *   producer_thread = kthread_create(producer_fn, NULL, "mb_producer");
       *   kthread_bind(producer_thread, 0);          // 绑 CPU0
       *   wake_up_process(producer_thread);
       *
       *   consumer_thread = kthread_create(consumer_fn, NULL, "mb_consumer");
       *   kthread_bind(consumer_thread, 1);          // 绑 CPU1
       *   wake_up_process(consumer_thread);
       */
	   producer_thread = kthread_create(producer_fn, NULL, "mb_producer");
	   kthread_bind(producer_thread, 0);
	   wake_up_process(producer_thread);
	   
	   consumer_thread = kthread_create(consumer_fn, NULL, "mb_consumer");
	   kthread_bind(consumer_thread, 1);
	   wake_up_process(consumer_thread);
	   
      return 0;
}

static void __exit mb_exit(void)
{
      if (producer_thread) kthread_stop(producer_thread);
      if (consumer_thread) kthread_stop(consumer_thread);
      pr_info("mb: unloaded. rounds=%d, mismatch=%d (use_barrier=%d)\n",
              atomic_read(&rounds), atomic_read(&mismatch), use_barrier);
}

module_init(mb_init);
module_exit(mb_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("3.1: memory barrinly)");