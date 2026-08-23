// SPDX-License-Identifier: GPL-2.0
/*
 * preempt_scope.c — D1：preempt_count 顯微鏡
 *
 * 把 CH8~CH10 看過的 preempt_count 各種值「主動印出來」驗證：
 *   一般 process   → 0x00000000
 *   preempt_disable → PREEMPT 欄 +1
 *   spin_lock       → PREEMPT 欄 再 +1
 *   tasklet 裡      → SOFTIRQ 欄 (0x100)   ← 對應你 A3 的 256
 *   (選) 按鈕 ISR   → HARDIRQ 欄 (0x10000) ← 對應你 A3 的 65536
 *
 * preempt_count 位元佈局 (preempt.h)：
 *   0x000f0000 HARDIRQ | 0x0000ff00 SOFTIRQ | 0x000000ff PREEMPT
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/preempt.h>     /* preempt_count / preempt_disable */
#include <linux/spinlock.h>
#include <linux/interrupt.h>   /* tasklet */
#include <linux/hardirq.h>     /* hardirq_count / softirq_count / in_interrupt */

static DEFINE_SPINLOCK(my_lock);

/* ── 顯微鏡鏡頭：把 preempt_count 拆成三欄印出來（已寫好，直接用）── */
static void dump_pc(const char *where)
{
      unsigned int pc = preempt_count();

      pr_info("gpio_pc: [%-14s] preempt_count=0x%08x  HARDIRQ=%lu SOFTIRQ=%lu PREEMPT=%u  in_interrupt=%ld irqs_disabled=%d\n",
              where,
              pc,
              hardirq_count() >> HARDIRQ_SHIFT,   /* 硬中斷巢狀層數 */
              softirq_count() >> SOFTIRQ_SHIFT,   /* 軟中斷層數 */
              pc & PREEMPT_MASK,                  /* 搶佔禁止計數 */
              in_interrupt(),
			  irqs_disabled());
}

/* ── 下半部：在 softirq 上下文印一次，看 SOFTIRQ 欄 ── */
static void my_tasklet_fn(struct tasklet_struct *t)   /* 5.10 簽名：tasklet_struct* */
{
      /* TODO 3：在這裡呼叫 dump_pc("tasklet")
       *   預期看到 SOFTIRQ=1 (preempt_count 含 0x100)，in_interrupt 非 0
       *   → 對應你 A3 看到的 256 / 0x101 */
	   dump_pc("tasklet");
}
static DECLARE_TASKLET(my_tasklet, my_tasklet_fn);    /* 5.10：只收 2 參數 */

static int __init pc_init(void)
{
      unsigned long flags;

      /* 點① 一般 process context —— 已示範，直接用 */
      dump_pc("normal");          /* 預期 0x00000000，三欄全 0 */

      /* TODO 1：關搶佔，量一次
       *   preempt_disable();
       *   dump_pc("preempt_dis");   ← 預期 PREEMPT=1 (0x00000001)
       *   preempt_enable();
       */
	   preempt_disable();
	   dump_pc("preempt_dis");
	   preempt_enable();

      /* TODO 2：拿 spinlock，量一次
       *   spin_lock_irqsave(&my_lock, flags);
       *   dump_pc("spinlock");      ← 預期 PREEMPT=1 (spin_lock 也是關搶佔)
       *   spin_unlock_irqrestore(&my_lock, flags);
       *
       *   進階：連續 preempt_disable() 再 spin_lock，看 PREEMPT 疊到 2
       */
	   
	   spin_lock(&my_lock);
	   dump_pc("spinlock");
	   spin_unlock(&my_lock);
	   
	   spin_lock_irqsave(&my_lock, flags);
	   dump_pc("spin_lock_irqsave");
	   spin_unlock_irqrestore(&my_lock, flags);

      /* 觸發 tasklet（它會在稍後的 softirq 跑，去印 SOFTIRQ 欄）*/
      tasklet_schedule(&my_tasklet);

      pr_info("gpio_pc: loaded\n");
      return 0;
}

static void __exit pc_exit(void)
{
      tasklet_kill(&my_tasklet);
      pr_info("gpio_pc: unloaded\n");
}

module_init(pc_init);
module_exit(pc_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D1: preempt_count microscope");