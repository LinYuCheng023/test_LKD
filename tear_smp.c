// SPDX-License-Identifier: GPL-2.0
/*
 * tear_smp.c — B1 雙核版：真並行撕裂（不加鎖 vs 加鎖）
 *
 * 两核各绑一核：writer 狂写多栏位结构、reader 狂读检查一致性。
 *   use_lock=0: 不加锁 → 两核真并行 → reader 读到 writer「写一半」→ 撕裂
 *   use_lock=1: spin_lock 保护 → 三栏一起读/写 → 永远一致
 *
 * ★ 跟单核 B1 不同：单核靠「中断插入」偶发撕裂；双核是「真并行」→ 撕裂率高很多。
 * ★ 撕裂是正确性问题（不是效能）→ A53 也一定看得出（不像屏障/false sharing 被温和淹没）。
 */
 
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/delay.h>

static int use_lock = 0;
module_param(use_lock, int, 0644);
MODULE_PARM_DESC(use_lock, "1=spinlock保护(不撕裂); 0=不加锁(撕裂)");

struct rec {
	unsigned long a, b, c;
};

static struct rec shared;
static DEFINE_SPINLOCK(rec_lock);

static struct task_struct *writer_t, *reader_t;
static atomic64_t tear = ATOMIC64_INIT(0);
static atomic64_t reads = ATOMIC64_INIT(0);

static int writer_fn(void *arg)
{
	unsigned long v;
	unsigned long flags;
	
	while (!kthread_should_stop()) {
		v++;
		if (use_lock) {
			spin_lock_irqsave(&rec_lock, flags);
			shared.a = v;
			shared.b = v;
			shared.c = v;
			spin_unlock_irqrestore(&rec_lock, flags);
		} else {
			shared.a = v;
			shared.b = v;
			shared.c = v;
		}
	}
	return 0;
}

static int reader_fn(void *arg)
{
	unsigned long a, b, c;
	unsigned long flags;
	
	while(!kthread_should_stop()) {
		if (use_lock) {
			spin_lock_irqsave(&rec_lock, flags);
			a = shared.a;
			b = shared.b;
			c = shared.c;
			spin_unlock_irqrestore(&rec_lock, flags);
		} else {
			a = shared.a;
			b = shared.b;
			c = shared.c;
		}
		
		atomic64_inc(&reads);
		if (a != b || b != c)
			atomic64_inc(&tear);
		
		 if (atomic64_read(&reads) % 50000000 == 0)
                      pr_info("tear_smp: use_lock=%d reads=%lld tear=%lld\n",
                              use_lock,
                              (long long)atomic64_read(&reads),
                              (long long)atomic64_read(&tear));
	}
	return 0;
}

static int __init ts_init(void)
{
      pr_info("tear_smp: loaded, use_lock=%d\n", use_lock);

      writer_t = kthread_create(writer_fn, NULL, "tear_writer");
      kthread_bind(writer_t, 0);            /* 绑 CPU0 */
      reader_t = kthread_create(reader_fn, NULL, "tear_reader");
      kthread_bind(reader_t, 1);            /* 绑 CPU1 */
      wake_up_process(writer_t);
      wake_up_process(reader_t);
      return 0;
}

static void __exit ts_exit(void)
{
      if (writer_t) kthread_stop(writer_t);
      if (reader_t) kthread_stop(reader_t);
      pr_info("tear_smp: unloaded. use_lock=%d reads=%lld tear=%lld\n",
              use_lock,
              (long long)atomic64_read(&reads),
              (long long)atomic64_read(&tear));
}

module_init(ts_init);
module_exit(ts_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("B1-SMP: true-parallel tearing (lock vs no-lock)");