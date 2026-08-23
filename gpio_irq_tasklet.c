// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_irq_tasklet.c — CH8 A3：tasklet 版下半部（已在板子驗證）
 *
 * 用 GPIO129 按鈕觸發，上半部 tasklet_schedule() 把活踢給 tasklet 下半部。
 * 目的：對照三種上下文，並親眼看「中斷上下文不能睡」的後果。
 *
 * ── 三種上下文的 in_interrupt() 實測值 ──────────────────────────────
 *   上半部(ISR)         in_interrupt()=65536 (0x10000 HARDIRQ)   不能睡
 *   tasklet 下半部       in_interrupt()=256   (0x100  SOFTIRQ)    不能睡  ★本檔
 *   threaded/workqueue   in_interrupt()=0     (process context)   可以睡
 *
 * ── 為什麼 tasklet 不能睡（本檔最重要的領悟）──────────────────────
 *   tasklet 沒有自己的執行緒，它「借」被中斷者的殼在跑（irq_exit → __do_softirq）。
 *   實測 backtrace：tasklet 竟跑在無辜行程 hunt_server(音訊擷取) 的 context 上——
 *   它只是剛好在那瞬間被按鈕中斷打斷。
 *   → 若在此 msleep()，被拖去睡的是那個無辜路人，不是「tasklet 自己」。
 *   → 中斷上下文根本沒有「該睡的人」，硬睡就是綁架路人陪葬。
 *   反觀 threaded/workqueue 有專屬 kthread，睡的是自己，故可睡。
 *
 * ── try_sleep=1 觀察到的兩層後果 ──────────────────────────────────
 *   might_sleep() → "sleeping function called from invalid context"（只警告，無害）
 *   msleep()      → "BUG: scheduling while atomic" + preempt_count 被腐蝕：
 *                   __do_softirq 稽核到 "huh, entered softirq with preempt_count
 *                   0x101, exited with 0x0?"（softirq 執行期間 preempt_count 恆定
 *                   的不變量被打破，kernel 只能硬掰回去、記污點 W，帶傷續跑）
 *
 * ── 實際會碰到的傷害（在這顆 AV/camera SoC 上）─────────────────────
 *   立即：持鎖又睡 → 死鎖 → watchdog 重啟
 *   延遲：核心資料(鏈表/記憶體 metadata)腐蝕 → 幾秒後在【無關模組】oops（兇手現場分離）
 *   持續：softirq 記帳亂 → 掉幀/破音、RTSP 斷線、timer 漂移
 *   debug：時好時壞、backtrace 指向受害者不指向兇手 → 最難追
 *   ★ 跑過 msleep 版後系統已帶傷，務必 reboot 再繼續。
 *
 * 目標平台：Linux 4.19 / NT98525 (arm 32-bit，單核)
 *
 * 用法：
 *   insmod gpio_irq_tasklet.ko              # 正常：看 tasklet in_interrupt()=256
 *   insmod gpio_irq_tasklet.ko try_sleep=1  # 示範不能睡（預設 might_sleep 安全版）
 *   rmmod  gpio_irq_tasklet
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>   /* request_irq / irqreturn_t / IRQF_* */
#include <linux/gpio.h>        /* gpio_request / gpio_to_irq */
#include <linux/workqueue.h>   /* 下半部：workqueue */
#include <linux/delay.h>       /* msleep */
#include <linux/hardirq.h>     /* in_interrupt() */

#define MY_GPIO 129            /* D_GPIO1 / FACTORY_DEFAULT_ID */
#define MY_NAME "ian_key"      /* 會顯示在 /proc/interrupts 最後一欄 */

static int my_irq = -1;

/*
 * 計數用 atomic_t：ISR(上半部) 和 work(下半部) 都會碰，
 * 用 atomic 就不必上鎖。若你有「非 atomic 的共享資料」被 ISR 和 process 同時碰，
 * 那就要用 spin_lock_irqsave / spin_unlock_irqrestore（§7.9）。
 */
static atomic_t top_count    = ATOMIC_INIT(0);   /* 上半部被呼叫幾次 */
static atomic_t bottom_count = ATOMIC_INIT(0);   /* 下半部被呼叫幾次 */

static int try_sleep = 0;
module_param(try_sleep, int, 0644);
MODULE_PARM_DESC(try_sleep, "1=call might_sleep() inside tasklet to prove it can't sleep");

/* ------------------------------------------------------------------ */
/* 下半部：workqueue handler —— 跑在 process context，可以睡、做重活    */
/* ------------------------------------------------------------------ */
static void my_tasklet_fn(struct tasklet_struct *t)   /* 5.10 簽名 */
{
	/*
	 * TODO 3：下半部（重活）。這裡是 process context —— 可以睡！
	 *   建議做：
	 *     atomic_inc(&bottom_count);
	 *     pr_info("gpio_irq: [下半部] work 開始, in_interrupt()=%ld (應為0=process context)\n",
	 *             in_interrupt());
	 *     msleep(50);   // ← 故意睡一下！證明下半部「可以睡」（ISR 絕對不行）
	 *     pr_info("gpio_irq: [下半部] work 完成, bottom_count=%d\n",
	 *             atomic_read(&bottom_count));
	 */
	atomic_inc(&bottom_count);
	pr_info("gpio_irq: [tasklet] in_interrupt()=%ld (★應非0=中斷上下文★), bottom_count=%d\n",
              in_interrupt(), atomic_read(&bottom_count));

	  if (try_sleep) {
			  pr_info("gpio_irq: [tasklet] 準備 might_sleep()（示範:這裡不准睡）\n");
			  msleep(50);   /* 會噴 "sleeping function called from invalid context" 但不當機 */
			  /* 若改成 msleep(50) 會更慘：直接 "scheduling while atomic" oops，可能要重開機 */
	  }

}
static DECLARE_TASKLET(my_tasklet, my_tasklet_fn);   /* 5.10：2 參數 */

/* ------------------------------------------------------------------ */
/* 上半部：ISR —— 跑在 interrupt context，不能睡，快進快出              */
/* ------------------------------------------------------------------ */
static irqreturn_t my_isr(int irq, void *dev)
{
	/*
	 * TODO 2：上半部（ISR 本體）。interrupt context —— 不能睡！只做急事、秒回。
	 *   建議做：
	 *     atomic_inc(&top_count);
	 *     pr_info("gpio_irq: [上半部] IRQ %d! in_interrupt()=%ld (應非0=interrupt context), top_count=%d\n",
	 *             irq, in_interrupt(), atomic_read(&top_count));
	 *     schedule_work(&my_work);   // ← 把重活丟給下半部（§7.3），秒回
	 *     return IRQ_HANDLED;        // ← 回傳：這中斷我處理了（§7.5）
	 *
	 *   ★ 這裡「絕對不能」msleep / mutex / kmalloc(GFP_KERNEL) —— 會睡就違反第⑤條！
	 *     想睡的事全部丟給上面的 my_work_handler 去做。
	 */
	atomic_inc(&top_count);
	pr_info("gpio_irq: [上半部] IRQ %d! in_interrupt()=%ld (應非0=interrupt context), top_count=%d\n",
			irq, in_interrupt(), atomic_read(&top_count));
	tasklet_schedule(&my_tasklet);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
static int __init gpio_irq_init(void)
{
	int ret;

	/* 1. 取得 GPIO（若已被 sysfs export 會 -EBUSY → 先 unexport，見檔頭說明）*/
	ret = gpio_request(MY_GPIO, MY_NAME);
	if (ret) {
		pr_err("gpio_irq: gpio_request(%d) 失敗: %d (試試先 echo %d > /sys/class/gpio/unexport)\n",
		       MY_GPIO, ret, MY_GPIO);
		return ret;
	}
	gpio_direction_input(MY_GPIO);   /* 中斷源要是輸入腳 */

	/* 2. GPIO 號 → IRQ 號（★ 129 不是 IRQ 號，一定要轉 ★）*/
	my_irq = gpio_to_irq(MY_GPIO);
	if (my_irq < 0) {
		pr_err("gpio_irq: gpio_to_irq(%d) 失敗: %d\n", MY_GPIO, my_irq);
		gpio_free(MY_GPIO);
		return my_irq;
	}
	pr_info("gpio_irq: gpio %d -> irq %d\n", MY_GPIO, my_irq);

	/*
	 * TODO 1：用 request_irq 註冊 ISR。
	 *   API：request_irq(irq, handler, flags, name, dev)
	 *     irq     = my_irq
	 *     handler = my_isr
	 *     flags   = IRQF_TRIGGER_FALLING   // 按下拉低 → 下降緣觸發
	 *     name    = MY_NAME                // 顯示在 /proc/interrupts
	 *     dev     = NULL                   // 非共享，NULL 即可
	 *   成功回 0；失敗要「gpio_free(MY_GPIO); return ret;」把資源還回去。
	 *
	 *   例：
	 *     ret = request_irq(my_irq, my_isr, IRQF_TRIGGER_FALLING, MY_NAME, NULL);
	 *     if (ret) {
	 *         pr_err("gpio_irq: request_irq 失敗: %d\n", ret);
	 *         gpio_free(MY_GPIO);
	 *         return ret;
	 *     }
	 */
	 ret = request_irq(my_irq, my_isr, IRQF_TRIGGER_FALLING, MY_NAME, NULL);
	 if(ret) {
		pr_err("gpio_irq: request_irq failed: %d\n",ret);
		gpio_free(MY_GPIO);
		return ret;
	}

	pr_info("gpio_irq: loaded. 按 FACTORY_DEFAULT 鈕觸發；cat /proc/interrupts | grep %s\n",
		MY_NAME);
	return 0;
}

static void __exit gpio_irq_exit(void)
{
	/*
	 * TODO 4：清理（反向拆解，順序很重要）：
	 *   1) free_irq(my_irq, NULL);       // 先停掉中斷（dev 要跟 request_irq 一致=NULL）
	 *   2) cancel_work_sync(&my_work);   // 等還沒跑完的下半部跑完，別留尾巴
	 *   3) gpio_free(MY_GPIO);           // 還回 GPIO
	 *   （先停中斷、再清下半部、最後還 GPIO —— 順序反了可能 ISR 又排入 work）
	 */
	 free_irq(my_irq, NULL);
	 tasklet_kill(&my_tasklet);
	 gpio_free(MY_GPIO);

	pr_info("gpio_irq: unloaded (top=%d, bottom=%d)\n",
		atomic_read(&top_count), atomic_read(&bottom_count));
}

module_init(gpio_irq_init);
module_exit(gpio_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("CH7 GPIO interrupt demo: top-half ISR + workqueue bottom-half");