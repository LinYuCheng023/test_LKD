// SPDX-License-Identifier: GPL-2.0
/*
 * kfifo_demo.c — CH6 §6.2 佇列（kfifo）實戰（骨架 + TODO）
 *
 * 生產者 / 消費者模型：
 *   生產者：echo 一個數字 > /sys/kernel/kfifo_demo/in   → kfifo_in 推入
 *   消費者：cat            /sys/kernel/kfifo_demo/out   → kfifo_out 取出（FIFO）
 *   狀態  ：cat            /sys/kernel/kfifo_demo/status → 看 len/avail/空滿
 *
 * 鷹架（kfifo 宣告 / sysfs / 鎖 / 錯誤處理）已搭好，
 * 4 個 TODO 是 kfifo 的核心動作，由你填：
 *   TODO 1：kfifo_alloc  （§6.2.2 建立）
 *   TODO 2：kfifo_in     （§6.2.3 推入，記得檢查回傳值！）
 *   TODO 3：kfifo_out    （§6.2.4 取出，記得檢查回傳值！）
 *   TODO 4：kfifo_free   （§6.2.6 釋放）
 *
 * 目標平台：Linux 5.4.x（NT98525 / QEMU vexpress-a9）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kfifo.h>     /* 本節主角 */
#include <linux/spinlock.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>    /* kstrtouint */

/* 佇列容量（bytes）。★ 必須是 2 的次方（§6.2.2 的坑）★
 * 128 bytes / 每個 unsigned int 4 bytes = 最多裝 32 個整數。 */
#define FIFO_SIZE 128

static struct kfifo   my_fifo;
static struct kobject *fifo_kobj;

/*
 * kfifo 的 in/out「本身不防併發」——多個 writer/reader 要自己序列化。
 * sysfs 可能被多個 process 同時 echo/cat，所以我們用 spinlock 保護（第⑥條）。
 * 全在 process context，沒有 ISR 會碰，用一般 spin_lock 即可。
 * （kfifo 另有 kfifo_in_spinlocked/kfifo_out_spinlocked 幫你包鎖，這裡手動鎖比較看得清楚。）
 */
static DEFINE_SPINLOCK(fifo_lock);

/* ------------------------------------------------------------------ */
/* 生產者：echo N > in                                                 */
/* ------------------------------------------------------------------ */
static ssize_t in_store(struct kobject *kobj, struct kobj_attribute *attr,
			const char *buf, size_t count)
{
	unsigned int val, n;
	int ret;

	ret = kstrtouint(buf, 10, &val);   /* 把字串轉成數字 */
	if (ret < 0)
		return ret;

	spin_lock(&fifo_lock);
	/*
	 * TODO 2：用 kfifo_in 把 val（sizeof(val) bytes）推入 my_fifo，
	 *         把回傳值（實際塞進的 bytes 數）接到 n。
	 *
	 *   API：unsigned int kfifo_in(fifo, from, len)
	 *        fifo = &my_fifo
	 *        from = &val
	 *        len  = sizeof(val)
	 */
	n = kfifo_in(&my_fifo, &val, sizeof(val));/* TODO 2：填這裡 */;
	spin_unlock(&fifo_lock);

	/* 關鍵：kfifo_in 回傳值可能 < len（佇列滿）！一定要檢查，不然資料默默掉。 */
	if (n != sizeof(val)) {
		pr_warn("kfifo_demo: FULL — value %u dropped (only %u bytes in)\n",
			val, n);
		return -ENOSPC;
	}

	pr_info("kfifo_demo: pushed %u\n", val);
	return count;
}

/* ------------------------------------------------------------------ */
/* 消費者：cat out（讀出來就從佇列消失）                                */
/* ------------------------------------------------------------------ */
static ssize_t out_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	unsigned int val, n;

	spin_lock(&fifo_lock);
	/*
	 * TODO 3：用 kfifo_out 從 my_fifo 取一個 unsigned int 到 val，
	 *         把回傳值接到 n。
	 *
	 *   API：unsigned int kfifo_out(fifo, to, len)
	 *        fifo = &my_fifo
	 *        to   = &val
	 *        len  = sizeof(val)
	 */
	n = kfifo_out(&my_fifo, &val, sizeof(val));/* TODO 3：填這裡 */;
	spin_unlock(&fifo_lock);

	/* 回傳值 < len 代表佇列空了（沒東西可取）。 */
	if (n != sizeof(val))
		return scnprintf(buf, PAGE_SIZE, "(empty)\n");

	pr_info("kfifo_demo: popped %u\n", val);
	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

/* ------------------------------------------------------------------ */
/* 狀態查詢（這個我幫你填好，當查詢類 API 的參考範例）                  */
/* ------------------------------------------------------------------ */
static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	unsigned int size, len, avail;
	int empty, full;

	spin_lock(&fifo_lock);
	size  = kfifo_size(&my_fifo);      /* 總容量 */
	len   = kfifo_len(&my_fifo);       /* 目前裝了多少 */
	avail = kfifo_avail(&my_fifo);     /* 還剩多少空位 */
	empty = kfifo_is_empty(&my_fifo);
	full  = kfifo_is_full(&my_fifo);
	spin_unlock(&fifo_lock);

	return scnprintf(buf, PAGE_SIZE,
		"size=%u len=%u avail=%u empty=%d full=%d  (bytes; 1 int = %zu bytes)\n",
		size, len, avail, empty, full, sizeof(unsigned int));
}

static struct kobj_attribute in_attr     = __ATTR(in,     0220, NULL,        in_store);
static struct kobj_attribute out_attr    = __ATTR(out,    0440, out_show,    NULL);
static struct kobj_attribute status_attr = __ATTR(status, 0440, status_show, NULL);

/* ------------------------------------------------------------------ */
static int __init kfifo_demo_init(void)
{
	int ret;

	/*
	 * TODO 1：用 kfifo_alloc 建立一個 FIFO_SIZE bytes 的佇列。
	 *
	 *   API：int kfifo_alloc(fifo, size, gfp_mask)  → 成功回 0、失敗回負值
	 *        fifo = &my_fifo
	 *        size = FIFO_SIZE   （★必須 2 的次方★）
	 *        gfp  = GFP_KERNEL
	 */
	ret = kfifo_alloc(&my_fifo, FIFO_SIZE, GFP_KERNEL);/* TODO 1：填這裡 */;
	if (ret)
		return ret;

	fifo_kobj = kobject_create_and_add("kfifo_demo", kernel_kobj);
	if (!fifo_kobj) {
		ret = -ENOMEM;
		goto err_fifo;
	}

	ret = sysfs_create_file(fifo_kobj, &in_attr.attr);
	if (ret)
		goto err_kobj;
	ret = sysfs_create_file(fifo_kobj, &out_attr.attr);
	if (ret)
		goto err_in;
	ret = sysfs_create_file(fifo_kobj, &status_attr.attr);
	if (ret)
		goto err_out;

	pr_info("kfifo_demo: loaded (size=%d bytes). echo N>in ; cat out ; cat status\n",
		FIFO_SIZE);
	return 0;

err_out:
	sysfs_remove_file(fifo_kobj, &out_attr.attr);
err_in:
	sysfs_remove_file(fifo_kobj, &in_attr.attr);
err_kobj:
	kobject_put(fifo_kobj);
err_fifo:
	kfifo_free(&my_fifo);   /* TODO 1 若失敗這裡其實還沒 alloc；填完 TODO 1 後這條路徑才有意義 */
	return ret;
}

static void __exit kfifo_demo_exit(void)
{
	/* 先拔 sysfs，確保之後不會再有 in/out 進來（反向拆解）*/
	sysfs_remove_file(fifo_kobj, &in_attr.attr);
	sysfs_remove_file(fifo_kobj, &out_attr.attr);
	sysfs_remove_file(fifo_kobj, &status_attr.attr);
	kobject_put(fifo_kobj);

	/*
	 * TODO 4：釋放 kfifo（kfifo_alloc 配的，要用 kfifo_free 還回去，否則洩漏，第③條）。
	 *
	 *   API：void kfifo_free(fifo)   fifo = &my_fifo
	 */
	/* TODO 4：填這裡 */
	kfifo_free(&my_fifo);
	pr_info("kfifo_demo: unloaded\n");
}

module_init(kfifo_demo_init);
module_exit(kfifo_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("LKD Ch6 kfifo producer/consumer demo");