#if 1
// SPDX-License-Identifier: GPL-2.0
/*
 * gpio_debounce.c — D6：按鍵去抖（timer + GPIO中斷 + spinlock）
 *
 * 串起：CH7 GPIO中斷 / CH11 動態定時器(timer_setup/mod_timer/del_timer_sync)
 *       CH10 spin_lock_irqsave / §11.3 迴繞安全 time_after
 *
 * 去抖模式：每個彈跳 edge → ISR mod_timer(推後20ms)；彈跳停20ms → timer 才到期 = 一次真按鍵。
 * 觀察：raw_count(彈跳邊緣總數) >> press_count(去抖後真正按鍵數)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/timer.h>       /* ★ timer_setup / mod_timer / del_timer_sync / from_timer */
#include <linux/jiffies.h>     /* time_after / msecs_to_jiffies */
#include <linux/spinlock.h>
#include <linux/hardirq.h>

#define MY_GPIO 129
#define MY_NAME "ian_debounce"
#define DEBOUNCE_MS 20

/* 裝置結構：timer 內嵌其中（4.19 慣例，取代舊的 .data 欄）*/
struct dbnc_dev {
      struct timer_list timer;      /* ★ 內嵌 timer */
      spinlock_t        lock;       /* 保護下面的共享資料（ISR 和 timer callback 都碰）*/
      int               my_irq;
      unsigned long     last_edge;  /* 最後一個 edge 的 jiffies（迴繞安全示範）*/
      unsigned int      raw_count;  /* ISR：每個彈跳 edge +1 */
      unsigned int      press_count;/* timer：去抖後真正按鍵 +1 */
};
static struct dbnc_dev ddev;

/* ── timer callback：彈跳停 20ms 後到期 = 一次真按鍵（softirq 上下文，不能睡）── */
static void debounce_timer_fn(struct timer_list *t)
{
      /* TODO 1：從內嵌 timer 反推裝置結構
       *   struct dbnc_dev *d = from_timer(d, t, timer);
       */
	   struct dbnc_dev *d = container_of(t, struct dbnc_dev, timer);
      unsigned long flags;

      /* TODO 2：拿鎖更新 press_count（跟 ISR 共享 → irqsave）
       *   spin_lock_irqsave(&d->lock, flags);
       *   d->press_count++;
       *   spin_unlock_irqrestore(&d->lock,
       *
       *   pr_info("debounce: ★真按鍵! presupt()=%ld\n",
       *           d->press_count, d->raw_count, in_interrupt());
       */
	   spin_lock_irqsave(&d->lock, flags);
	   d->press_count++;
	   spin_unlock_irqrestore(&d->lock, flags);
	   
	   pr_info("debounce: ★真按鍵! press=%u (期間 raw=%u), in_interrupt()=%ld\n",
        d->press_count, d->raw_count, in_interrupt());
}

/* ── ISR：每個彈跳 edge 都進來（快進快出）── */
static irqreturn_t debounce_isr(int irq, void *dev)
{
      struct dbnc_dev *d = dev;
      unsigned long flags;

      spin_lock_irqsave(&d->lock, flags);
      d->raw_count++;
      d->last_edge = jiffies;   /* 記錄這個 edge 的時刻 */
      spin_unlock_irqrestore(&d->lock, flags);

      /* TODO 3：把 timer 推後到「現在 + 20ms」
       *   → 彈跳期間反覆呼叫，timer 一直被推後，直到彈跳停才會真正到期
       *   mod_timer(&d->timer, jiffies + msecs_to_jiffies(DEBOUNCE_MS));
       *
       *   ★ 為什麼用 mod_timer 不用 del+add：mod_timer 是原子的、多核安全（§11.7）
       */
	   mod_timer(&d->timer, jiffies + msecs_to_jiffies(DEBOUNCE_MS));

      return IRQ_HANDLED;
}

static int __init dbnc_init(void)
{
      int ret;

      spin_lock_init(&ddev.lock);

      ret = gpio_request(MY_GPIO, MY_NAME);
      if (ret) { pr_err("debounce: gpio_request 失敗 %d\n", ret); return ret; }
      gpio_direction_input(MY_GPIO);

      ddev.my_irq = gpio_to_irq(MY_GPIO);
      if (ddev.my_irq < 0) { gpio_free(MY_GPIO); return ddev.my_irq; }

      /* TODO 4：初始化 timer（4.19 新 API）
       *   timer_setup(&ddev.timer, debounce_timer_fn, 0);
       *   （不用先設 .expires，等 ISR 第一次 mod_timer 才啟動）
       */
	   timer_setup(&ddev.timer, debounce_timer_fn, 0);

      ret = request_irq(ddev.my_irq, debounce_isr, IRQF_TRIGGER_FALLING, MY_NAME, &ddev);
      if (ret) { pr_err("debounce: request_irq 失敗 %d\n", ret); gpio_free(MY_GPIO); return ret; }

     pr_info("debounce: loaded，按按鈕看 raw vs press（去抖 %dms）\n", DEBOUNCE_MS);
      return 0;
}

static void __exit dbnc_exit(void)
{
      /* 清理順序：先停中斷（不再 mod_timer），再同步刪 timer，最後還 GPIO */
      free_irq(ddev.my_irq, &ddev);

      /* TODO 5：安全刪除 timer
       *   del_timer_sync(&ddev.timer);   ← 等「正在跑的 callback」跑完才返回（§11.7）
       */
		timer_delete_sync(&ddev.timer);
      gpio_free(MY_GPIO);
      pr_info("debounce: unloaded (raw=%u, press=%u)\n", ddev.raw_count, ddev.press_count);
}

module_init(dbnc_init);
module_exit(dbnc_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D6: GPIO button debounce (timer_setup/mod_timer + spinlock)");
#else
// SPDX-License-Identifier: GPL-2.0
/*
 * key_debounce.c - 按键去抖练习骨架 (Linux 5.4 新 API)
 * 覆盖: CH7(中断) + CH8(下半部/上下文) + CH10(同步) + CH11(定时器)
 *
 * 用法(以 device tree GPIO 为例, 见文末说明):
 *   把 GPIO 号改成你板子上的按键脚
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/timer.h>        // CH11: 定时器
#include <linux/spinlock.h>     // CH10: 自旋锁
#include <linux/jiffies.h>      // CH11: jiffies / msecs_to_jiffies
#include <linux/delay.h>        // CH11: (示范用) delay

/* ===================== 可调参数 ===================== */
#define KEY_GPIO        129              /* ★改成你板子的按键 GPIO 号 */
#define DEBOUNCE_MS     20              /* 去抖时间: 20ms */

/* ===================== 设备结构 =====================
 * ★把定时器"内嵌"进设备结构 → 配合 from_timer (=container_of)
 *   这是 5.4 新 API 的核心模式 (取代旧的 timer.data 塞指针)
 */
struct key_dev {
    int             irq;                /* 中断号 */
    int             gpio;               /* GPIO 号 */

    struct timer_list debounce_timer;   /* CH11: 去抖定时器(内嵌) */
    spinlock_t      lock;               /* CH10: 保护下面的共享数据 */

    /* 共享数据: ISR 写, 定时器回调读/清 → 必须加锁 */
    unsigned long   raw_irq_count;      /* 原始中断次数(含抖动) */
    unsigned long   key_press_count;    /* 去抖后的真实按键次数 */
    unsigned long   last_irq_jiffies;   /* 上次中断的时间戳 */
	
	int last_stable_state;
};

static struct key_dev *g_dev;

/* ===================== 定时器回调 =====================
 * CH11: 定时器超时后跑这里
 * CH8:  跑在软中断上下文(TIMER_SOFTIRQ) → 不能睡!
 * 到这里 = 抖动已经停了 DEBOUNCE_MS → 确认是一次真实按键
 */
static void debounce_timer_cb(struct timer_list *t)
{
    /* ★5.4 新 API: from_timer 从 timer_list* 反推宿主结构
     *   本质就是 container_of (你 CH8 workqueue 学过的套路)
     */
    struct key_dev *dev = container_of(t, struct key_dev, debounce_timer);
    unsigned long flags;
    unsigned long raw, presses;
    int gpio_val;

    /* CH8: 这里是软中断上下文,不能睡!
     * 如果要做会睡的事(比如 i2c 上报), 要 schedule_work 丢 workqueue
     */

    /* 读一下 GPIO 当前电平, 确认按键状态 (可选, 看你按键是高有效还是低有效) */
    gpio_val = gpio_get_value(dev->gpio);
	if (gpio_val == dev->last_stable_state)
		return ;
	
	dev->last_stable_state = gpio_val;

    /* CH10: 和 ISR 共享数据 → 加锁
     * 对方(ISR)在硬中断上下文, 所以理论上要 irqsave;
     * 但定时器回调本身在软中断上下文, ISR 会打断它 → 用 irqsave 最安全
     */
    spin_lock_irqsave(&dev->lock, flags);
    dev->key_press_count++;
    raw = dev->raw_irq_count;
    presses = dev->key_press_count;
    spin_unlock_irqrestore(&dev->lock, flags);

	if(gpio_val == 0)
		pr_info("key_debounce: [KEY DOWN] press #%lu (gpio=%d, raw_irqs=%lu)\n",presses, gpio_val, raw);
	else
		pr_info("key_debounce: [KEY UP  ] press #%lu (gpio=%d, raw_irqs=%lu)\n",presses, gpio_val, raw);
}

/* ===================== 中断处理程序 (上半部) =====================
 * CH7: ISR, 硬中断上下文, 要快速返回, 不能睡
 * 这里只做一件事: 重设去抖定时器
 */
static irqreturn_t key_isr(int irq, void *data)
{
    struct key_dev *dev = data;
    unsigned long flags;

    /* CH10: 更新共享计数, 和定时器回调共享 → irqsave
     * (ISR 已在硬中断, irqsave 会存/关本地中断)
     */
    spin_lock_irqsave(&dev->lock, flags);
    dev->raw_irq_count++;               /* 每个抖动都数(含噪声) */
    dev->last_irq_jiffies = jiffies;    /* CH11: 记时间戳 */
    spin_unlock_irqrestore(&dev->lock, flags);

    /* ★CH11 核心: mod_timer 重设定时器
     *   - 抖动期间反复调用 → 定时器不断被往后推 → 到不了期
     *   - 抖动停止 20ms 后 → 定时器终于到期 → 回调触发
     *   ★用 mod_timer, 绝不能用 "del_timer + add_timer" (多核 race!)
     */
	
	del_timer(&dev->debounce_timer);
	dev->debounce_timer.expires = jiffies + msecs_to_jiffies(DEBOUNCE_MS);
	add_timer(&dev->debounce_timer);
	/*
	mod_timer(&dev->debounce_timer,
			  jiffies + msecs_to_jiffies(DEBOUNCE_MS));
*/
    return IRQ_HANDLED;
}

/* ===================== 模块初始化 ===================== */
static int __init key_debounce_init(void)
{
    struct key_dev */;
    int ret;

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);   /* GFP_KERNEL: init 在进程上下文, 可睡 */
    if (!dev)
        return -ENOMEM;

    dev->gpio = KEY_GPIO;
    spin_lock_init(&dev->lock);                /* CH10: 初始化锁 */

    /* ★CH11 5.4 新 API: timer_setup 取代 init_timer + 手填 function
     *   参数: (定时器, 回调, flags)
     */
    timer_setup(&dev->debounce_timer, debounce_timer_cb, 0);

    /* ---- 申请 GPIO ---- */
    ret = gpio_request(dev->gpio, "debounce_key");
    if (ret) {
        pr_err("key_debounce: gpio_request %d failed: %d\n", dev->gpio, ret);
        goto err_free;
    }
    gpio_direction_input(dev->gpio);
	
	dev->last_stable_state = gpio_get_value(dev->gpio);

    /* ---- GPIO 转中断号 ---- */
    dev->irq = gpio_to_irq(dev->gpio);
    if (dev->irq < 0) {
        pr_err("key_debounce: gpio_to_irq failed: %d\n", dev->irq);
        ret = dev->irq;
        goto err_gpio;
    }

    /* ---- CH7: 申请中断 ----
     * 双边沿触发(按下+松开都触发), 你可按需改成:
     *   IRQF_TRIGGER_FALLING (按下)
     *   IRQF_TRIGGER_RISING  (松开)
     *   IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING (双边沿)
     *
     * 注: 这里用普通 request_irq(上半部). 如果去抖回调要"睡",
     *     应改用 threaded IRQ (见文末进阶).
     */
	ret = request_irq(dev->irq, key_isr,
					  IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
					  "debounce_key", dev);

    if (ret) {
        pr_err("key_debounce: request_irq failed: %d\n", ret);
        goto err_gpio;
    }

    g_dev = dev;
    pr_info("key_debounce: loaded. gpio=%d irq=%d debounce=%dms\n",
            dev->gpio, dev->irq, DEBOUNCE_MS);
    return 0;

err_gpio:
    gpio_free(dev->gpio);
err_free:
    kfree(dev);
    return ret;
}

/* ===================== 模块卸载 ===================== */
static void __exit key_debounce_exit(void)
{
    struct key_dev *dev = g_dev;

    /* ★清理顺序很重要! */

    /* 1. 先释放中断 → 之后不会再有新的 ISR → 不会再 mod_timer
     *    (如果先删定时器, ISR 可能又把它加回来 → race)
     */
    free_irq(dev->irq, dev);

    /* 2. CH11: 删除定时器, 用 del_timer_sync
     *    - 保证"将来不触发" + "等正在跑的回调跑完"
     *    - ★del_timer_sync 前不能持有回调会拿的锁(否则死锁)!
     *      这里没持锁, 安全
     *    - del_timer_sync 不能在中断上下文用(这里是进程上下文, OK)
     */
    del_timer_sync(&dev->debounce_timer);

    gpio_free(dev->gpio);

    pr_info("key_debounce: unloaded. total real presses=%lu, raw_irqs=%lu\n",
            dev->key_press_count, dev->raw_irq_count);

    kfree(dev);
}

module_init(key_debounce_init);
module_exit(key_debounce_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("your name");
MODULE_DESCRIPTION("Key debounce practice: CH7 IRQ + CH8 context + CH10 lock + CH11 timer");
#endif