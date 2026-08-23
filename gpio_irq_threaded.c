// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_irq_threaded.c — CH7 中斷實戰（threaded IRQ + spinlock，已完成）
 *
 * 在 NT98525 上，用 GPIO129（FACTORY_DEFAULT 按鈕）當中斷源，
 * 按下（下降緣）觸發 threaded IRQ：上半部秒回，下半部丟給 kernel thread 做。
 *
 * 這支把 CH7 整章串起來，並跑過 4 個實驗（皆已在板子上驗證）：
 *   A2 threaded IRQ：request_threaded_irq，上半部回 IRQ_WAKE_THREAD，
 *                    下半部是專屬 kernel thread（pid 固定），可 msleep。
 *   A1 下半部合併：拿掉 IRQF_ONESHOT + 按鈕彈跳 → top_count > bottom_count，
 *                  肇因是 threaded IRQ 的 IRQTF_RUNTHREAD bit 會合併多次喚醒。
 *   C1 映射驗證：/proc/interrupts 的計數 = 上半部次數；hwirq 33 →(radix tree)→ irq 289。
 *   B1 spin_lock_irqsave：ISR 與 reader kthread 共搶多欄位結構 {a,b,c}。
 *                  use_lock=0 → reader 讀到撕裂 (a≠b≠c)；use_lock=1 → 恆一致。
 *
 * ★ 上半部(ISR)     = interrupt context，不能睡，快進快出（in_interrupt()!=0）
 * ★ 下半部(thread)  = process context，可以睡、做重活   （in_interrupt()==0）
 * ★ 臨界區越短越好：持鎖/關中斷時只拷值，printk 等慢動作一律放到鎖外面。
 *
 * 模組參數：
 *   use_lock  1=用 spin_lock_irqsave 保護共享結構（預設）；0=不保護，觀察撕裂
 *             可動態切：echo 0|1 > /sys/module/gpio_irq_threaded/parameters/use_lock
 *
 * 目標平台：Linux 4.19 / NT98525 (arm 32-bit，單核)
 *
 * 使用：
 *   echo 129 > /sys/class/gpio/unexport         # 若沒 export 過會報錯，忽略即可
 *   insmod gpio_irq_threaded.ko use_lock=0      # 看 B1 撕裂
 *   cat /proc/interrupts | grep ian_key         # 289 f0070000.gpio Edge ian_key
 *   # → 按 FACTORY_DEFAULT 按鈕幾下，看 dmesg ←
 *   echo 1 > /sys/module/gpio_irq_threaded/parameters/use_lock   # 撕裂停止
 *   rmmod gpio_irq_threaded
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>   /* request_irq / irqreturn_t / IRQF_* */
#include <linux/gpio.h>        /* gpio_request / gpio_to_irq */
#include <linux/workqueue.h>   /* （舊 workqueue 版遺留，threaded 版已不用）*/
#include <linux/delay.h>       /* msleep */
#include <linux/hardirq.h>     /* in_interrupt() */
#include <linux/kthread.h>
#include <linux/spinlock.h>

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
struct shared_rec {
	unsigned long a, b, c;
};
static struct shared_rec rec;
static DEFINE_SPINLOCK(rec_lock);
static atomic_t tear_count = ATOMIC_INIT(0);
static struct task_struct *reader_thread;

static int use_lock = 1;
module_param(use_lock, int, 0644);
MODULE_PARM_DESC(use_lock, "1=protect with spin_lock_irqsave, 0=race");

/* ------------------------------------------------------------------ */
/* 下半部：threaded IRQ handler —— 跑在專屬 kernel thread(process       */
/*         context)，可以睡、做重活。上半部回 IRQ_WAKE_THREAD 才會跑到。 */
/* ------------------------------------------------------------------ */
static irqreturn_t my_thread_fn(int irq, void *dev)
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
	pr_info("gpio_irq: [下半部/thread] 開始, in_interrupt()=%ld (應為0=process context), pid=%d\n",
	              in_interrupt(), current->pid);
	msleep(50);   
	pr_info("gpio_irq: [下半部/thread] 完成, bottom_count=%d\n",
	              atomic_read(&bottom_count));
	return IRQ_HANDLED;
}
//static DECLARE_WORK(my_work, my_work_handler);   /* 定義一個 work，綁上面的 handler */

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
	unsigned long flags;
	unsigned long v;
	
	atomic_inc(&top_count);
	pr_info("gpio_irq: [上半部] IRQ %d! in_interrupt()=%ld (應非0=interrupt context), top_count=%d\n",
			irq, in_interrupt(), atomic_read(&top_count));
	//schedule_work(&my_work);
	
	v = (unsigned long)atomic_read(&top_count);
	
	if(use_lock)
		spin_lock_irqsave(&rec_lock, flags);
	
	rec.a = v;
	rec.b = v;
	rec.c = v;
	
	if(use_lock)
		spin_unlock_irqrestore(&rec_lock, flags);
	
	return IRQ_WAKE_THREAD;
}

static int reader_fn(void *data)
{
	unsigned long flags;
	unsigned long a, b, c;
	unsigned long rounds = 0;
	
	while(!kthread_should_stop()) {
		int i;
		for(i = 0;i < 2000; i++) {
			if(use_lock)
				spin_lock_irqsave(&rec_lock, flags);
			
			a = rec.a;
			udelay(10);
			b = rec.b;
			c = rec.c;
			
			if(use_lock)
				spin_unlock_irqrestore(&rec_lock, flags);
			
			if(a != b || b != c) {
				atomic_inc(&tear_count);
				pr_info("gpio_irq: [reader] ★撕裂! a=%lu b=%lu c=%lu  (tear#%d)\n",
						a, b, c, atomic_read(&tear_count));
			}
		}
		if(++rounds % 50 == 0)
			pr_info("gpio_irq: [reader] use_lock=%d tear_count=%d (last a=%lu b=%lu c=%lu)\n",
					use_lock, atomic_read(&tear_count), a, b, c);
		msleep(20);
	}
	return 0;
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
	 //ret = request_irq(my_irq, my_isr, IRQF_TRIGGER_FALLING, MY_NAME, NULL);
	 ret = request_threaded_irq(my_irq, my_isr, my_thread_fn, IRQF_TRIGGER_FALLING, MY_NAME, NULL);
	 if(ret) {
		pr_err("gpio_irq: request_irq failed: %d\n",ret);
		gpio_free(MY_GPIO);
		return ret;
	}
	
	reader_thread = kthread_run(reader_fn, NULL, "ian_reader");
	if(IS_ERR(reader_thread)) {
		pr_err("gpio_irq: reader kthread  建立失敗\n");
		reader_thread = NULL;
	}

	pr_info("gpio_irq: loaded. 按 FACTORY_DEFAULT 鈕觸發；cat /proc/interrupts | grep %s\n",
		MY_NAME);
	return 0;
}

static void __exit gpio_irq_exit(void)
{
	/*
	 * 清理（反向拆解，順序很重要）：
	 *   1) free_irq()      // 先停中斷；threaded 版會自動等 thread_fn 跑完並收掉那條執行緒
	 *      （所以不用 cancel_work_sync —— 那是舊 workqueue 版才需要的）
	 *   2) kthread_stop()  // 停掉 B1 的 reader kthread
	 *   3) gpio_free()     // 最後還回 GPIO
	 */
	 free_irq(my_irq, NULL);
	if(reader_thread)
		kthread_stop(reader_thread);
	pr_info("gpio_irq: [B1] 最終 tear_count=%d (use_lock=%d)\n",atomic_read(&tear_count), use_lock);
	gpio_free(MY_GPIO);

	pr_info("gpio_irq: unloaded (top=%d, bottom=%d)\n",
		atomic_read(&top_count), atomic_read(&bottom_count));
}

module_init(gpio_irq_init);
module_exit(gpio_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("CH7 GPIO interrupt demo: threaded IRQ + spin_lock_irqsave (A2/A1/C1/B1)");