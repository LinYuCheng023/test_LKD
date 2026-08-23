// SPDX-License-Identifier: GPL-2.0
/*
 * spin_deadlock.c — D5：spinlock 自死鎖（會卡死 → watchdog 重啟！）
 *
 * ★★★ 警告：insmod 這支會讓單核板子整台凍結，靠硬體 watchdog 重開機。★★★
 * ★★★ 執行前存好所有工作。★★★
 *
 * 原理（§9.3 自死鎖 + §10.2 spinlock 不可遞迴）：
 *   spin_lock 拿第一次 → 成功
 *   spin_lock 拿第二次（同一把）→ 空轉等「自己持有的鎖」被釋放
 *      → 但自己正卡在空轉、永遠不會走到 unlock → 永遠等下去
 *      → 單核：唯一的 CPU 被佔死 → 整台凍結 → watchdog 超時重啟
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(my_lock);

static int __init dl_init(void)
{
      pr_info("deadlock: 準備自死鎖…板子即將凍結、watchdog 重啟\n");

      spin_lock(&my_lock);
      pr_info("deadlock: 第一次 spin_lock 成功（已持有）\n");

      /* TODO：再拿一次同一把鎖 —— 這行之後就回不來了
       *   spin_lock(&my_lock);     ← ★ 自死鎖！空轉等自己持有的鎖
       *   pr_info("deadlock: 這行永遠印不出來\n");
       */
	   spin_lock(&my_lock);
	   pr_info("deadlock: 這行永遠印不出來\n");

      /* 下面這些永遠執行不到 */
      spin_unlock(&my_lock);
      pr_info("deadlock: loaded（永遠到不了這裡）\n");
      return 0;
}

static void __exit dl_exit(void)
{
      pr_info("deadlock: unloaded\n");   /* 也永遠到不了（因為 init 就卡死了）*/
}

module_init(dl_init);
module_exit(dl_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D5: spinlock self-deadlock (WILL HANG -> watchdog reboot)");