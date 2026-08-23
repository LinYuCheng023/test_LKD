// SPDX-License-Identifier: GPL-2.0
/*
 * mutex_atomic.c — D4：睡眠鎖(mutex)在中斷上下文會被抓
 *
 * 在 tasklet（softirq 上下文）裡 mutex_lock → mutex_lock 內含 might_sleep()
 * → CONFIG_DEBUG_ATOMIC_SLEEP 抓到「原子上下文呼叫睡眠函式」→ 印 BUG 警告
 *
 * 對比 A3：A3 是 msleep 在 tasklet；這裡是 mutex_lock 在 tasklet —— 同一類錯。
 * 差別：might_sleep 只警告不當機（鎖是空的，實際沒真睡）。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/hardirq.h>

static DEFINE_MUTEX(my_mutex);

static void my_tasklet_fn(struct tasklet_struct *t)   /* 5.10 簽名 */
{
      pr_info("mtxatom: [tasklet] in_interrupt()=%ld，準備 mutex_lock（違規！）\n",
              in_interrupt());

      /* TODO 1：在中斷上下文拿 mutex —— 這行會觸發 might_sleep 警告
       *   mutex_lock(&my_mutex);
       *   pr_info("mtxatom: [tasklet] 拿到鎖了（居然沒當機，因為鎖是空的沒真睡）\n");
       *   mutex_unlock(&my_mutex);
       */
	   mutex_lock(&my_mutex);
	   pr_info("mtxatom: [tasklet] 拿到鎖了（居然沒當機，因為鎖是空的沒真睡）\n");
	   mutex_unlock(&my_mutex);
}
static DECLARE_TASKLET(my_tasklet, my_tasklet_fn);   /* 5.10：2 參數 */

static int __init ma_init(void)
{
      pr_info("mtxatom: loaded，排一個 tasklet 去踩雷\n");

      /* TODO 2：觸發 tasklet
       *   tasklet_schedule(&my_tasklet);
       */
	   tasklet_schedule(&my_tasklet);
      return 0;
}

static void __exit ma_exit(void)
{
      tasklet_kill(&my_tasklet);
      pr_info("mtxatom: unloaded\n");
}

module_init(ma_init);
module_exit(ma_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D4: mutex_lock in interrupt context (should warn)");