// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_irq_demo.c — CH7 中斷實戰（骨架 + TODO）
 *
 * 在 NT98525 上，用 GPIO129（FACTORY_DEFAULT 按鈕）當中斷源，
 * 按下（下降緣）觸發 ISR，ISR 把重活丟給 workqueue 下半部處理。
 *
 * 這支把 CH7 整章串起來：
 *   §7.4 request_irq 註冊 / §7.5 ISR 寫法+回傳值 /
 *   §7.3 上半部(ISR)＋下半部(workqueue) / §7.6 中斷上下文 /
 *   §7.8 /proc/interrupts 驗證 / §7.9 free_irq + in_interrupt()
 *
 * ★ 上半部(ISR)   = interrupt context，不能睡，快進快出
 * ★ 下半部(work)  = process context，可以睡，做重活
 *   → 程式裡兩邊都印 in_interrupt()，你會看到 ISR 非0、work 是0！
 *
 * 4 個 TODO：
 *   TODO 1：request_irq 註冊 ISR（§7.4）
 *   TODO 2：ISR 上半部本體（§7.5 + §7.3）
 *   TODO 3：workqueue 下半部本體（§7.3 的重活，證明「可以睡」）
 *   TODO 4：free_irq + cancel_work_sync 清理
 *
 * 目標平台：Linux 5.4.x / NT98525 (arm 32-bit)
 *
 * 使用（填完 TODO 後）：
 *   # gpio129 若已被 sysfs export，先還給我們用：
 *   echo 129 > /sys/class/gpio/unexport      # 若沒 export 過，這行會報錯，忽略即可
 *   insmod gpio_irq_demo.ko
 *   cat /proc/interrupts | grep ian_key      # 看到你的中斷出現（計數 0）
 *   # → 按 FACTORY_DEFAULT 按鈕幾下 ←
 *   cat /proc/interrupts | grep ian_key      # 計數跳了！
 *   dmesg                                     # 看上半部/下半部的 log 交錯
 *   rmmod gpio_irq_demo
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

/* ------------------------------------------------------------------ */
/* 下半部：workqueue handler —— 跑在 process context，可以睡、做重活    */
/* ------------------------------------------------------------------ */
static void my_work_handler(struct work_struct *work)
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
	pr_info("gpio_irq: [下半部] work 開始, in_interrupt()=%ld (應為0=process context)\n",
	              in_interrupt());
	msleep(50);   
	pr_info("gpio_irq: [下半部] work 完成, bottom_count=%d\n",
	              atomic_read(&bottom_count));

}
static DECLARE_WORK(my_work, my_work_handler);   /* 定義一個 work，綁上面的 handler */

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
	schedule_work(&my_work);
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
	 cancel_work_sync(&my_work);
	 gpio_free(MY_GPIO);

	pr_info("gpio_irq: unloaded (top=%d, bottom=%d)\n",
		atomic_read(&top_count), atomic_read(&bottom_count));
}

module_init(gpio_irq_init);
module_exit(gpio_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("CH7 GPIO interrupt demo: top-half ISR + workqueue bottom-half");