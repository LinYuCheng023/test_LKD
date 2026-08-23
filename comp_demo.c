// SPDX-License-Identifier: GPL-2.0
/*
 * comp_demo.c — D2：completion 親手做（複刻 VPE 的 complete/wait）
 *
 * 一個 kthread 用 wait_for_completion() 睡著等；
 * 按鈕 ISR 用 complete() 喚醒它。
 *
 * 對照 VPE：
 *   VPE 的 process   wait_for_completion(&ad
 *   VPE 的 ISR       complete(&vpe_completion)             ← 你的按鈕 ISR
 *
 * 觀察重點：
 *   等待方(kthread)  in_interrupt()==0  → process context，可以睡  ★
 *   通知方(ISR)      in_interrupt()!=0  → 中斷上下文（不能睡，但 complete() 不睡，OK）
 *   ps | grep ian    → 沒按時 kthread 在睡 (S 狀態)，不吃 CPU
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/kthread.h>      /* ← 確認有這行 */
#include <linux/sched.h>        /* ← 補這行（current / task_struct）*/
#include <linux/completion.h>
#include <linux/hardirq.h>
int my_irq = -1;
static struct task_struct *waiter_thread;

#define MY_GPIO 129
#define MY_NAME "ian_comp"

/* ── completion 變數：靜態建立 ── */
static DECLARE_COMPLETION(my_comp);
/* 動態版寫法（擇一）：static struct completion my_comp; init_completion(&my_comp); */

static atomic_t wake_count = ATOMIC_INIT(0);

/* ── 通知方：按鈕 ISR（中斷上下文）── */
static irqreturn_t my_isr(int irq, void *dev)
{
      /* TODO 1：喚醒等待的 kthread
       *   pr_info("comp: [ISR] 按鈕! in_interrupt()=%ld，發 complete\n", in_interrupt());
       *   complete(&my_comp);        ← 喚醒睡在 wait_for_completion 的 kthread
       *   return IRQ_HANDLED;
       *
       *   ★ 注意：這裡是中斷上下文，只能用 complete()（它不睡）；
       *     絕不能在這裡 wait_for_completion（那會睡 → 崩潰）
       */
	   pr_info("comp: [ISR] 按鈕! in_interrupt()=%ld，發 complete\n", in_interrupt());
	   complete(&my_comp);
	   
      return IRQ_HANDLED;
}

/* ── 等待方：kthread（process context，可以睡）── */
static int waiter_fn(void *data)
{
      while (!kthread_should_stop()) {
              pr_info("comp: [waiter] 開始等待 (in_interrupt()=%ld, pid=%d) …睡著\n",
                      in_interrupt(), current->pid);

              /* TODO 2：睡著等事件
               *   wait_for_completion(&my_comp);   ← 卡在這，直到 ISR complete()
               *
               *   進階：改用 wait_for_completion_interruptible(&my_comp)
               *   → 回傳值檢查是否被信號中斷（更正確，能被 kill）
               */
				wait_for_completion(&my_comp);
              atomic_inc(&wake_count);
              pr_info("comp: [waiter] ★被喚醒！wake_count=%d\n", atomic_read(&wake_count));
      }
      return 0;
}

static int __init comp_init(void)
{
      int ret;

      /* 按鈕設定（你熟的流程）*/
      ret = gpio_request(MY_GPIO, MY_NAME);
      if (ret) { pr_err("comp: gpio_request 失敗 %d\n", ret); return ret; }
      gpio_direction_input(MY_GPIO);

      my_irq = gpio_to_irq(MY_GPIO);
      if (my_irq < 0) { gpio_free(MY_GPIO); return my_irq; }

      ret = request_irq(my_irq, my_isr, IRQF_TRIGGER_FALLING, MY_NAME, NULL);
      if (ret) { pr_err("comp: request_irq 失敗 %d\n", ret); gpio_free(MY_GPIO); return ret; }

      /* TODO 3：啟動等待 kthread
       *   waiter_thread = kthread_run(waiter_fn, NULL, "ian_waiter");
       *   if (IS_ERR(waiter_thread)) { ... 清理 ... }
       */
	waiter_thread = kthread_run(waiter_fn, NULL, "ian_waiter");
      if (IS_ERR(waiter_thread)) {
              pr_err("comp: kthread 建立失敗\n");
              free_irq(my_irq, NULL);
              gpio_free(MY_GPIO);
              return PTR_ERR(waiter_thread);
      }

      pr_info("comp: loaded，按 FACTORY_DEFAULT 鈕喚醒 waiter；ps | grep ian_waiter\n");
      return 0;
}

static void __exit comp_exit(void)
{
      /* 清理順序：先停中斷，再喚醒/停 kthread（否則它可能永遠睡在 wait）*/
      free_irq(my_irq, NULL);

      if (waiter_thread) {
              complete(&my_comp);            /* ★ 補一發 complete，讓卡在 wait 的 kthread 醒來，才停得掉 */
              kthread_stop(waiter_thread);
      }
      gpio_free(MY_GPIO);
      pr_info("comp: unloaded (wake_count=%d)\n", atomic_read(&wake_count));
}

module_init(comp_init);
module_exit(comp_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D2: completion demo (wait_for_completion / complete)");