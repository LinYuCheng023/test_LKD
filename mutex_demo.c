// SPDX-License-Identifier: GPL-2.0
/*
 * mutex_demo.c — D3：mutex（睡眠鎖）vs spinlock（忙等）
 *
 * 兩個 kthread 搶同一把 mutex，持鎖者故意 msleep(2s)。觀察：
 *   ① 持鎖者可以 msleep —— mutex 不關搶佔/中斷，持鎖時睡覺合法（spinlock 絕不行！）
 *   ② 等待者「睡著」等鎖 —— top 看它 0% CPU（不像 spinlock 忙等燒 CPU）
 *   ③ 兩者絕不同時在臨界區 —— in_cs 永遠不會 >1
 *
 * 對比 B1 的 spinlock：
 *   spinlock 持鎖時 msleep → scheduling while atomic 崩潰（你 A3 看過）
 *   mutex    持鎖時 msleep → 完全合法
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/sched.h>

static DEFINE_MUTEX(my_mutex);
static struct task_struct *workers[2];

/* 臨界區佔用計數：進去 +1、出來 -1。任何時刻應該永遠 <=1，>1 就是互斥失效 */
static atomic_t in_cs = ATOMIC_INIT(0);

static DEFINE_MUTEX(cross_mutex);

static int evil = 0;
module_param(evil, int , 0644);
MODULE_PARM_DESC(evil, "1=worker0 unlocks a mutex held by worker1 (should warn)");

static int worker_fn(void *data)
{
      int id = (int)(long)data;   /* 0 或 1，代表 worker A / B */
      int occ;

      while (!kthread_should_stop()) {
              pr_info("mtx: [worker%d] 想拿鎖…（拿不到會睡著等）\n", id);

              /* TODO 1：拿 mutex（拿不到就睡，不燒 CPU）
               *   mutex_lock(&my_mutex);
               */
				mutex_lock(&my_mutex);
              /* ── 進入臨界區 ── */
              occ = atomic_inc_return(&in_cs);
              pr_info("mtx: [worker%d] ★拿到鎖，進臨界區 (in_cs=%d, pid=%d)%s\n",
                      id, occ, current->pid,
                      occ > 1 ? "  ← ★★互斥失效！不該發生★★" : "");

              /* ★ 持鎖時 msleep —— mutex 合法！（spinlock 這樣做會崩潰）*/
			  if(evil) {
				if(id == 1) {
					mutex_lock(&cross_mutex);
					pr_info("mtx: [worker1] 鎖住 cross_mutex，持有中…\n");
                    msleep(1000);
				} else {
					msleep(500);
					pr_info("mtx: [worker0] ★試圖解鎖 worker1 持有的 cross_mutex（違規！）\n");
                    mutex_unlock(&cross_mutex);   /* ★ DEBUG_MUTEXES 應在此抓包 */
				}
			  }
              msleep(2000);

              atomic_dec(&in_cs);
              pr_info("mtx: [worker%d] 離開臨界區，放鎖\n", id);
              /* ── 離開臨界區 ── */

              /* TODO 2：放 mutex
               *   mutex_unlock(&my_mutex);
               */
			   
			   mutex_unlock(&my_mutex);

              msleep(300);   /* 休息一下再搶下一輪，讓另一個有機會 */
      }
      return 0;
}

static int __init mtx_init(void)
{
      /* TODO 3：建立兩個 worker kthread
       *   workers[0] = kthread_run(worker_fn, (void *)0L, "ian_mtxA");
       *   workers[1] = kthread_run(worker_fn, (void *)1L, "ian_mtxB");
       *   （簡化起見這裡先不做 IS_ERR 檢查，實務上要做）
       */
	   workers[0] = kthread_run(worker_fn, (void *)0L, "ian_mtxA");
	   workers[1] = kthread_run(worker_fn, (void *)1L, "ian_mtxB");

      pr_info("mtx: loaded，觀察 dmesg；top 看 ian_mtxA/B 是否 0%% CPU（睡著等）\n");
      return 0;
}

static void __exit mtx_exit(void)
{
      int i;
      for (i = 0; i < 2; i++) {
              if (workers[i])
                      kthread_stop(workers[i]);   /* kthread_stop 會等它跑完當前迴圈 */
      }
      pr_info("mtx: unloaded\n");
}

module_init(mtx_init);
module_exit(mtx_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("D3: mutex sleeping-lock vs spinlock");