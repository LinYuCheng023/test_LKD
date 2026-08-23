// SPDX-License-Identifier: GPL-2.0
/*
 * false_sharing.c — cache bouncing / false sharing 量測（雙核專屬，改進版）
 *
 * 改進點（比第一版更容易看出差異）：
 *   ① 起跑同步：两核到齐才一起冲 → 保证「同时」抢同一 cache line
 *   ② 加大迭代 + WRITE_ONCE：强制每次碰记忆体，不让编译器优化掉
 *   ③ 各自计时自己的纯循环时间（不含建 thread/等待的杂讯）
 *
 * share=1: 两计数器同 cache line → false sharing → cache line 在两核间 ping-pong → 慢
 * share=0: 两计数器分开(cacheline_aligned) → 不 bouncing → 快
 * ★ 单核无此现象；这台双核才量得出。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/cache.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/delay.h>

static int share = 1;
module_param(share, int, 0644);
MODULE_PARM_DESC(share, "1=同cache line(false sharing); 0=分开");

#define ITERS 500000000L   /* 5 亿次，够久才看得出 cache 效应 */

/* 情况A：两计数器相邻 → 同一 cache line */
struct shared_layout {
	unsigned long counter0;
	unsigned long counter1;
};
static struct shared_layout shared_cnt;

/* 情况B：各自对齐到不同 cache line */
struct aligned_layout {
	unsigned long counter0 ____cacheline_aligned;
	unsigned long counter1 ____cacheline_aligned;
};
static struct aligned_layout aligned_cnt;

static struct task_struct *t0, *t1;
static struct completion done0, done1;
static atomic_t ready = ATOMIC_INIT(0);   /* ★ 起跑同步计数 */
static s64 us0, us1;

static int worker_fn(void *arg)
{
	long id = (long)arg;
	long i;
	volatile unsigned long *cnt;
	ktime_t s, e;

	if (share)
		cnt = (id == 0) ? &shared_cnt.counter0 : &shared_cnt.counter1;
	else
		cnt = (id == 0) ? &aligned_cnt.counter0 : &aligned_cnt.counter1;

	/* ★ 起跑同步：我到了 → 等另一个也到 → 两个同时冲 */
	atomic_inc(&ready);
	while (atomic_read(&ready) < 2) {
		if (kthread_should_stop()) return 0;
		cpu_relax();
	}

	/* ↓ 两核从这里几乎同时开始狂加 → 真的「同时」抢 cache line */
	s = ktime_get();
	for (i = 0; i < ITERS; i++)
		WRITE_ONCE(*cnt, READ_ONCE(*cnt) + 1);   /* 强制碰记忆体 */
	e = ktime_get();

	if (id == 0) { us0 = ktime_to_us(ktime_sub(e, s)); complete(&done0); }
	else         { us1 = ktime_to_us(ktime_sub(e, s)); complete(&done1); }

	while (!kthread_should_stop())
		msleep(20);
	return 0;
}

static int __init fs_init(void)
{
	init_completion(&done0);
	init_completion(&done1);
	atomic_set(&ready, 0);

	pr_info("false_sharing: loaded, share=%d, iters=%ld, cacheline=%d bytes\n",
		share, ITERS, cache_line_size());

	t0 = kthread_create(worker_fn, (void *)0L, "fs_w0");
	kthread_bind(t0, 0);
	t1 = kthread_create(worker_fn, (void *)1L, "fs_w1");
	kthread_bind(t1, 1);
	wake_up_process(t0);
	wake_up_process(t1);

	wait_for_completion(&done0);
	wait_for_completion(&done1);

	pr_info("false_sharing: share=%d 純循環耗時 w0=%lld us, w1=%lld us  ← %s\n",
		share, us0, us1,
		share ? "同cache line(false sharing)" : "分开");
	return 0;
}

static void __exit fs_exit(void)
{
	if (t0) kthread_stop(t0);
	if (t1) kthread_stop(t1);
	pr_info("false_sharing: unloaded\n");
}

module_init(fs_init);
module_exit(fs_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("false sharing measurement, improved (SMP)");
