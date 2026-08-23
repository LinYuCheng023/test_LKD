// SPDX-License-Identifier: GPL-2.0
/*
 * tear_crash.c — 「8% 撕裂 → 越界 → 崩溃」赌局 demo（教育用）
 *
 * ★★★ 会故意崩溃/损坏记忆体 → 大概率 watchdog 重启。存好工作！★★★
 *
 * 证明「感觉还好的 8% 撕裂」= 8% 机率引爆致命越界：
 *   writer(CPU0)：同时写 {ptr, len}，在(小buf,小len)和(大buf,大len)间切换
 *   reader(CPU1)：读 {ptr, len}，做 memset(ptr, 0, len)
 *     → 撕裂读到「小buf 的 ptr」配「大 len」→ memset 越界写爆 → 崩溃
 *
 * use_lock=1: 加锁 → ptr/len 一起读写 → 永远配对 → 不越界（对照组）
 * use_lock=0: 不加锁 → 8% 撕裂 → 迟早 memset 越界 → 炸
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>

static int use_lock = 0;
module_param(use_lock, int, 0644);
MODULE_PARM_DESC(use_lock, "1=spinlock保护(不撕裂); 0=不加锁(撕裂)");

#define SMALL_SIZE 8
#define BIG_SIZE 65536

static char *small_buf, *big_buf;
struct desc {
	char *ptr;
	unsigned long len;
};

static struct desc shared;
static DEFINE_SPINLOCK(lock);

static struct task_struct *writer_t, *reader_t;


static int writer_fn(void *arg)
{
	int toggle = 0;
	unsigned long flags;
	
	while (!kthread_should_stop()) {
		toggle ^= 1;
		if (use_lock) {
			spin_lock_irqsave(&lock, flags);
			if (toggle) { shared.ptr = small_buf; shared.len = SMALL_SIZE; }
			else	    { shared.ptr = big_buf;   shared.len = BIG_SIZE;   }
			spin_unlock_irqrestore(&lock, flags);
		} else {
			if (toggle) { WRITE_ONCE(shared.ptr, small_buf); WRITE_ONCE(shared.len, SMALL_SIZE); }
			else	    { WRITE_ONCE(shared.ptr, big_buf);   WRITE_ONCE(shared.len, BIG_SIZE);   }
		}
	}
	return 0;
}

static int reader_fn(void *arg)
{
	char *p;
	unsigned long l;
	unsigned long flags;
	
	while(!kthread_should_stop()) {
		if (use_lock) {
			spin_lock_irqsave(&lock, flags);
			p = shared.ptr;
			l = shared.len;
			spin_unlock_irqrestore(&lock, flags);
		} else {
			p = (char *)READ_ONCE(shared.ptr);
			l = READ_ONCE(shared.len);
		}
		
		if (p)
			memset(p, 0, l);
	}
	return 0;
}

static int __init ts_init(void)
{
      small_buf = kmalloc(SMALL_SIZE, GFP_KERNEL);
	  big_buf   = kmalloc(BIG_SIZE,   GFP_KERNEL);
	  if (!small_buf || !big_buf) {
			  kfree(small_buf); kfree(big_buf);
			  return -ENOMEM;
	  }
	  shared.ptr = big_buf;
	  shared.len = BIG_SIZE;

	  pr_info("tear_crash: loaded, use_lock=%d %s\n", use_lock,
			  use_lock ? "(加锁，安全)" : "(不加锁，赌它炸)");

	  writer_t = kthread_create(writer_fn, NULL, "tc_writer");
	  kthread_bind(writer_t, 0);
	  reader_t = kthread_create(reader_fn, NULL, "tc_reader");
	  kthread_bind(reader_t, 1);
	  wake_up_process(writer_t);
	  wake_up_process(reader_t);
	  return 0;
}

static void __exit ts_exit(void)
{
      if (writer_t) kthread_stop(writer_t);
      if (reader_t) kthread_stop(reader_t);
      kfree(small_buf);
      kfree(big_buf);
      pr_info("tear_crash: unloaded\n");
}
module_init(ts_init);
module_exit(ts_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("tearing -> OOB write -> crash (educational gamble)");