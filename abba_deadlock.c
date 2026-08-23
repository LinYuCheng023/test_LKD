// SPDX-License-Identifier: GPL-2.0
/*
 * abba_deadlock.c — D5 雙核版：ABBA 死鎖（兩核各持一鎖互等）
 *
 * ★★★ 警告：會讓兩顆核都卡死自旋 → 可能整台卡 → watchdog 重啟。先存工作！★★★
 *
 * 雙核獨有的死鎖（單核做不出 ABBA，因為單核不會兩 thread 真並行持鎖）：
 *   threadA(CPU0): lock(A) → 想 lock(B)
 *   threadB(CPU1): lock(B) → 想 lock(A)
 *   → 互等對方手上的鎖 → 雙核同時卡死 → 死鎖
 *
 * 對比自死鎖：這個要「兩核真並行」才會發生，是雙核專屬。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/delay.h>

static DEFINE_SPINLOCK(lockA);
static DEFINE_SPINLOCK(lockB);
static struct task_struct *tA, *tB;

/* threadA（綁 CPU0）：先 A 後 B */
static int threadA_fn(void *arg)
{
      unsigned long flags;
      pr_info("abba: [A@CPU%d] 要拿 lockA\n", smp_processor_id());
      spin_lock_irqsave(&lockA, flags);
      pr_info("abba: [A] 拿到 lockA，睡一下讓 B 也拿到 lockB\n");

      mdelay(100);   /* ★ 故意等，讓 B 有時間拿到 lockB → 製造 ABBA 時機 */

      pr_info("abba: [A] 想拿 lockB…（此時 B 應該持著 lockB）→ 卡死\n");
      spin_lock(&lockB);   /* ★ 卡在這：B 持著 lockB 不放 */

      /* 到不了這裡 */
      spin_unlock(&lockB);
      spin_unlock_irqrestore(&lockA, flags);
      return 0;
}

/* threadB（綁 CPU1）：先 B 後 A（★反序！這就是 ABBA）*/
static int threadB_fn(void *arg)
{
      unsigned long flags;
      pr_info("abba: [B@CPU%d] 要拿 lockB\n", smp_processor_id());
      spin_lock_irqsave(&lockB, flags);
      pr_info("abba: [B] 拿到 lockB，睡一下讓 A 也拿到 lockA\n");

      mdelay(100);

      pr_info("abba: [B] 想拿 lockA…（此時 A 應該持著 lockA）→ 卡死\n");
      spin_lock(&lockA);   /* ★ 卡在這：A 持著 lockA 不放 */

      spin_unlock(&lockA);
      spin_unlock_irqrestore(&lockB, flags);
      return 0;
}

static int __init abba_init(void)
{
      pr_info("abba: loaded，準備製造 ABBA 死鎖（兩核）\n");

      tA = kthread_create(threadA_fn, NULL, "abba_A");
      kthread_bind(tA, 0);            /* 綁 CPU0 */
      wake_up_process(tA);

      tB = kthread_create(threadB_fn, NULL, "abba_B");
      kthread_bind(tB, 1);            /* 綁 CPU1 */
      wake_up_process(tB);

      return 0;   /* init 立刻返回，insmod 不卡；死鎖發生在兩個 kthread */
}

static void __exit abba_exit(void)
{
      if (tA) kthread_stop(tA);
      if (tB) kthread_stop(tB);
      pr_info("abba: unloaded\n");
}

module_init(abba_init);
module_exit(abba_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D5-SMP: ABBA deadlock across two cores");