// SPDX-License-Identifier: GPL-2.0
/*
 * idr_demo.c — CH6 §6.3 映射（idr）實戰（骨架 + TODO）
 *
 * ★ 用「5.4 的新 API」，不是書上那套 2.6 舊 API（idr_pre_get/idr_get_new 已刪除）★
 *
 * idr = 「整數 ID → 指標」的映射，強項是自動幫你生唯一 ID。
 * 這個 demo：分配一個 ID 綁定字串、用 ID 查回字串、用 ID 刪除、卸載時全清。
 *
 *   分配：echo name > /sys/kernel/idr_demo/alloc   → idr_alloc 生一個 id 綁上 name
 *   查找：echo id   > /sys/kernel/idr_demo/find    → idr_find 用 id 找回 name
 *   刪除：echo id   > /sys/kernel/idr_demo/remove  → idr_remove + kfree
 *   (結果都印到 dmesg)
 *
 * 鷹架（idr 宣告 / sysfs / 鎖 / kmalloc / 錯誤處理）已搭好，
 * 4 個 TODO 是新 API 的核心動作：
 *   TODO 1：idr_alloc          （§6.3.2 分配，一行取代舊的兩步 do-while）
 *   TODO 2：idr_find           （§6.3.3 查找）
 *   TODO 3：idr_remove         （§6.3.4 刪除）
 *   TODO 4：idr_for_each_entry （清理走訪）+ idr_destroy
 *
 * 目標平台：Linux 5.4.x（NT98525 / QEMU vexpress-a9）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/idr.h>       /* 本節主角 */
#include <linux/slab.h>      /* kmalloc / kfree */
#include <linux/mutex.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>    /* kstrtoint / strscpy */

/* 每個節點：idr 會把「一個 id」對應到「一個 idr_item 指標」*/
struct idr_item {
	int  id;
	char name[32];
};

/* DEFINE_IDR：一步「定義 + 初始化」一個 idr（新 API，省掉 idr_init 呼叫）*/
static DEFINE_IDR(demo_idr);

/*
 * idr 的操作「需要呼叫者自己上鎖」（idr 內部不自動同步）。
 * 因為 idr_alloc(GFP_KERNEL) 可能睡覺配記憶體，所以這裡用 mutex（可睡）而非 spinlock。
 * ——這正好呼應第⑤條：要在鎖內配可能睡的記憶體 → 用能睡的鎖（mutex），不能用 spinlock。
 */
static DEFINE_MUTEX(demo_lock);

static struct kobject *idr_kobj;

/* ------------------------------------------------------------------ */
/* 分配：echo name > alloc                                             */
/* ------------------------------------------------------------------ */
static ssize_t alloc_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	struct idr_item *item;
	int id;
	size_t len;

	item = kmalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;

	strscpy(item->name, buf, sizeof(item->name));
	len = strlen(item->name);
	if (len && item->name[len - 1] == '\n')   /* 去掉 echo 的換行 */
		item->name[len - 1] = '\0';

	mutex_lock(&demo_lock);
	/*
	 * TODO 1：用 idr_alloc 分配一個 id 綁定 item，範圍從 0 起、無上限。
	 *
	 *   API：int idr_alloc(idr, ptr, start, end, gfp)
	 *        idr   = &demo_idr
	 *        ptr   = item
	 *        start = 0
	 *        end   = 0        （0 = 無上限）
	 *        gfp   = GFP_KERNEL
	 *   回傳：成功 = 分到的 id（≥0）；失敗 = 負錯誤碼
	 */
	id = idr_alloc(&demo_idr, item, 0, 0, GFP_KERNEL);/* TODO 1：填這裡 */;
	mutex_unlock(&demo_lock);

	if (id < 0) {                 /* -ENOMEM / -ENOSPC */
		kfree(item);          /* 分配失敗，剛 kmalloc 的要還回去（第③條）*/
		return id;
	}

	item->id = id;
	pr_info("idr_demo: allocated id=%d name=%s\n", id, item->name);
	return count;
}

/* ------------------------------------------------------------------ */
/* 查找：echo id > find                                                */
/* ------------------------------------------------------------------ */
static ssize_t find_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct idr_item *item;
	int id, ret;

	ret = kstrtoint(buf, 10, &id);
	if (ret < 0)
		return ret;

	mutex_lock(&demo_lock);
	/*
	 * TODO 2：用 idr_find 拿 id 對應的指標到 item。
	 *
	 *   API：void *idr_find(idr, id)   找不到回 NULL
	 *        idr = &demo_idr, id = id
	 */
	item = idr_find(&demo_idr, id);/* TODO 2：填這裡 */;
	mutex_unlock(&demo_lock);

	if (!item)
		pr_info("idr_demo: id=%d not found\n", id);
	else
		pr_info("idr_demo: id=%d -> name=%s\n", id, item->name);
	return count;
}

/* ------------------------------------------------------------------ */
/* 刪除：echo id > remove                                              */
/* ------------------------------------------------------------------ */
static ssize_t remove_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct idr_item *item;
	int id, ret;

	ret = kstrtoint(buf, 10, &id);
	if (ret < 0)
		return ret;

	mutex_lock(&demo_lock);
	item = idr_find(&demo_idr, id);       /* 先找出指標，等下要 kfree 它 */
	if (item) {
		/*
		 * TODO 3：用 idr_remove 把 id 從映射中移除。
		 *
		 *   API：void idr_remove(idr, id)
		 *        idr = &demo_idr, id = id
		 *   注意：idr_remove 只從映射移除，不會 kfree 你的 item（跟 list_del 一樣！）
		 *         → 移除後要自己 kfree(item)（下面已幫你寫）。
		 */
		/* TODO 3：填這裡 */
		idr_remove(&demo_idr, id);
	}
	mutex_unlock(&demo_lock);

	if (!item) {
		pr_info("idr_demo: id=%d not found\n", id);
		return -ENOENT;
	}

	kfree(item);   /* 已從 idr 移除，沒人能再拿到這指標了 → 出鎖後 kfree 安全 */
	pr_info("idr_demo: removed id=%d\n", id);
	return count;
}

static struct kobj_attribute alloc_attr  = __ATTR(alloc,  0220, NULL, alloc_store);
static struct kobj_attribute find_attr   = __ATTR(find,   0220, NULL, find_store);
static struct kobj_attribute remove_attr = __ATTR(remove, 0220, NULL, remove_store);

/* ------------------------------------------------------------------ */
static int __init idr_demo_init(void)
{
	int ret;

	idr_kobj = kobject_create_and_add("idr_demo", kernel_kobj);
	if (!idr_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(idr_kobj, &alloc_attr.attr);
	if (ret)
		goto err_kobj;
	ret = sysfs_create_file(idr_kobj, &find_attr.attr);
	if (ret)
		goto err_alloc;
	ret = sysfs_create_file(idr_kobj, &remove_attr.attr);
	if (ret)
		goto err_find;

	pr_info("idr_demo: loaded. echo name>alloc ; echo id>find ; echo id>remove\n");
	return 0;

err_find:
	sysfs_remove_file(idr_kobj, &find_attr.attr);
err_alloc:
	sysfs_remove_file(idr_kobj, &alloc_attr.attr);
err_kobj:
	kobject_put(idr_kobj);
	return ret;
}

static void __exit idr_demo_exit(void)
{
	struct idr_item *item;
	int id;

	/* 先拔 sysfs（反向拆解）*/
	sysfs_remove_file(idr_kobj, &alloc_attr.attr);
	sysfs_remove_file(idr_kobj, &find_attr.attr);
	sysfs_remove_file(idr_kobj, &remove_attr.attr);
	kobject_put(idr_kobj);

	mutex_lock(&demo_lock);
	/*
	 * TODO 4：用 idr_for_each_entry 走訪 demo_idr 裡「還沒被刪」的每個 item，
	 *         把它 kfree 掉；走訪完再 idr_destroy 釋放 idr 內部結構。
	 *
	 *   API：idr_for_each_entry(idr, entry, id) { ... }
	 *        idr = &demo_idr, entry = item, id = id
	 *   慣例：先 for_each_entry 釋放「外部物件（item）」，最後 idr_destroy 一次收掉 idr 本體。
	 *   （走訪中只是 kfree 外部 item、沒有 idr_remove，所以不需要 _safe 版）
	 *
	 *   例：
	 *      idr_for_each_entry(&demo_idr, item, id) {
	 *          kfree(item);
	 *      }
	 *      idr_destroy(&demo_idr);
	 */
	/* TODO 4：填這裡 */
	idr_for_each_entry(&demo_idr, item, id) {
		kfree(item);
	}
	idr_destroy(&demo_idr);
	mutex_unlock(&demo_lock);

	pr_info("idr_demo: unloaded\n");
}

module_init(idr_demo_init);
module_exit(idr_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("LKD Ch6 idr demo (modern 5.4 API)");