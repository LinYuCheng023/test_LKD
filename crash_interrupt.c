#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/preempt.h>     // 為了 preempt_disable/preempt_enable

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("Ian's Deep Dive: Hardcore Atomic Sleep 3.0");

static int __init ian_crash_init(void)
{
    pr_info("猫猫：[Init] 3.0 絕對能載入作死模組，出發！\n");

    // 1. 關閉搶佔。這會讓當前 CPU 的 preempt_count 變成 1。
    // 這在核心裡被定義為 Atomic Context (原子上下文)，
    // 其安全限制跟中斷上下文（in_interrupt）一模一樣 —— 絕對不能睡覺！
    pr_info("猫猫：[Init] 正在進入原子上下文 (關閉搶佔)...\n");
    preempt_disable(); 

    // 2. 驗證看看我們是不是真的進去了
    // Note: preempt_count() > 0 就代表我們處於不能睡眠的 atomic 狀態
    pr_info("猫猫：[Init] 當前 preempt_count = %d (只要 > 0 就是原子狀態)\n", preempt_count());

    // 3. 準備引爆！在原子上下文中強行呼叫 msleep
    pr_info("猫猫：[Init] 💣 準備引爆！在禁搶佔狀態下呼叫 msleep(100)...\n");
    
    msleep(100);               // <--- 這裡排程器一定會抓狂！

    // 4. 清理現場 (如果居然沒死的話)
    preempt_enable();
    pr_info("猫猫：[Init] 居然沒死？這不科學！\n");

    return 0;
}

static void __exit ian_crash_exit(void)
{
    pr_info("猫猫：[Exit] 模組卸載。\n");
}

module_init(ian_crash_init);
module_exit(ian_crash_exit);