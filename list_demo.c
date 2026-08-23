// SPDX-License-Identifier: GPL-2.0
/*
 * ian_list_demo.c — CH6 §6.1 鏈表實戰（骨架 + TODO）
 *
 * 目標：把 CH3~CH6 學的 list_head 一整套串起來，做一個能動態加/刪的鏈表。
 *       鷹架（struct / sysfs / kmalloc / 鎖）我搭好了，
 *       4 個 TODO 是這幾節的核心動作，由你填：
 *         TODO 1：list_add_tail        （§6.1.5 新增）
 *         TODO 2：list_for_each_entry  （§6.1.6 走訪）
 *         TODO 3：list_for_each_entry_safe + list_del + kfree（§6.1.6 邊走邊刪）
 *         TODO 4：cleanup 清空整條鏈表 （同 TODO 3 的全清版）
 *
 * 目標平台：Linux 5.4.x（NT98525 / QEMU vexpress-a9）
 *
 * 玩法（填完 TODO 後）：
 *   insmod ian_list_demo.ko
 *   dmesg                                   # init 會塞 3 個種子節點並 dump
 *   echo redfox  > /sys/kernel/ian_list/add # 加一個節點
 *   echo 2       > /sys/kernel/ian_list/del # 刪掉 id=2 的節點
 *   dmesg                                   # 每次加/刪都會重印整條鏈表
 *   rmmod ian_list_demo                     # cleanup 清乾淨（看 dmesg 不該有殘留）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>      /* kmalloc / kfree */
#include <linux/list.h>      /* 本章主角：list_head 與一票 list_* API */
#include <linux/spinlock.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>    /* strscpy */

/* 我們的資料 struct：把 list_head「嵌」進來（侵入式，§6.1.4）*/
struct ian_item {
	int               id;
	char              name[32];
	struct list_head  node;     /* ← 鏈結能力嵌在這，靠 container_of 反推回 ian_item */
};

/* 鏈表頭：LIST_HEAD 一步「定義 + 初始化」一個當入口的 list_head（§6.1.4）*/
static LIST_HEAD(ian_list);

/*
 * 保護鏈表的鎖（保命守則第⑥條）。
 * 注意：_safe 版只防「自己迴圈內刪」，不防併發 → 共享鏈表仍要鎖。
 * 這裡所有存取都在 process context（sysfs / init / exit），沒有 ISR 會碰，
 * 所以用一般 spin_lock 即可；若有中斷處理也會改它，才升級成 spin_lock_irqsave。
 */
static DEFINE_SPINLOCK(ian_lock);

static int            next_id = 1;
static struct kobject *ian_kobj;

/* ------------------------------------------------------------------ */
/* 走訪：把整條鏈表印到 dmesg                                          */
/* ------------------------------------------------------------------ */
static void print_all(void)
{
	struct ian_item *it;

	spin_lock(&ian_lock);
	pr_info("ian_list: ---- dump ----\n");

	if (list_empty(&ian_list)) {
		pr_info("ian_list: (empty)\n");
	} else {
		/*
		 * TODO 2：用 list_for_each_entry 走訪 ian_list，
		 *         對每個節點 pr_info 印出 it->id 和 it->name。
		 *
		 *   API：list_for_each_entry(pos, head, member)
		 *        pos    = it           （ian_item*，迴圈變數）
		 *        head   = &ian_list
		 *        member = node         （list_head 在 struct 裡的「成員名」）
		 *
		 *   印的格式可參考：pr_info("ian_list:   id=%d name=%s\n", it->id, it->name);
		 */
		list_for_each_entry(it, &ian_list, node) {
			pr_info("ian_list: id = %d, name = %s\n",it->id, it->name);
		}
	}

	spin_unlock(&ian_lock);
}

/* ------------------------------------------------------------------ */
/* 新增一個節點到鏈表尾（FIFO）                                        */
/* ------------------------------------------------------------------ */
static int add_item(const char *name)
{
	struct ian_item *it;

	/* 先 kmalloc —— 一定要在「拿鎖之前」做，因為 GFP_KERNEL 可能睡（第⑤條）*/
	it = kmalloc(sizeof(*it), GFP_KERNEL);
	if (!it)
		return -ENOMEM;

	it->id = next_id++;
	strscpy(it->name, name, sizeof(it->name));
	/* 備註：用 list_add_tail 加入時，list_* 會幫 node 接好 next/prev，
	 *       所以這裡「不需要」先 INIT_LIST_HEAD(&it->node)。 */

	spin_lock(&ian_lock);
	/*
	 * TODO 1：把 it->node 用 list_add_tail 加到 ian_list 的尾巴。
	 *
	 *   API：list_add_tail(new, head)
	 *        new  = &it->node
	 *        head = &ian_list
	 *   （想做成 stack 就改用 list_add；這裡要 FIFO 所以用 _tail）
	 */
	list_add_tail(&it->node, &ian_list);
	//list_add(&it->node, &ian_list);
	/*
	struct list_head *new = &it->node;
	//struct list_head *prev = ian_list.prev;
	//struct list_head *next = &ian_list;
	struct list_head *prev = &ian_list;
	struct list_head *next = ian_list.next;
	
	new->next = next;
	new->prev = prev;
	next->prev = new;
	prev->next = new;
	*/
	
	spin_unlock(&ian_lock);

	pr_info("ian_list: added id=%d name=%s\n", it->id, it->name);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 刪除 id 相符的節點                                                  */
/* ------------------------------------------------------------------ */
static int del_item(int id)
{
	struct ian_item *it, *tmp;
	int found = 0;

	spin_lock(&ian_lock);
	/*
	 * TODO 3：用 list_for_each_entry_safe 走訪，找到 it->id == id 的節點，
	 *         把它 list_del 後 kfree，並設 found = 1，然後 break。
	 *
	 *   API：list_for_each_entry_safe(pos, n, head, member)
	 *        pos = it, n = tmp, head = &ian_list, member = node
	 *   提醒：list_del 只「移除」不釋放 → 還要自己 kfree(it)（§6.1.5）。
	 *         為什麼這裡非用 _safe 不可？因為迴圈內要 kfree 當前節點。
	 */
	 list_for_each_entry_safe(it, tmp, &ian_list, node) {
		if(it->id == id) {
			list_del(&it->node);
			kfree(it);
			found = 1;
			break;
		}
	 }

	spin_unlock(&ian_lock);

	if (found)
		pr_info("ian_list: deleted id=%d\n", id);
	else
		pr_info("ian_list: id=%d not found\n", id);
	return found ? 0 : -ENOENT;
}

/* ------------------------------------------------------------------ */
/* sysfs：echo name > add  /  echo id > del                            */
/* ------------------------------------------------------------------ */
static ssize_t add_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	char name[32];
	size_t len;

	strscpy(name, buf, sizeof(name));
	len = strlen(name);
	if (len && name[len - 1] == '\n')   /* 去掉 echo 帶的換行 */
		name[len - 1] = '\0';
	if (name[0] == '\0')
		return -EINVAL;

	add_item(name);
	print_all();
	return count;
}

static ssize_t del_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	int id, ret;

	ret = kstrtoint(buf, 10, &id);
	if (ret < 0)
		return ret;

	del_item(id);
	print_all();
	return count;
}

/* write-only（0220）：show 給 NULL */
static struct kobj_attribute add_attr = __ATTR(add, 0220, NULL, add_store);
static struct kobj_attribute del_attr = __ATTR(del, 0220, NULL, del_store);

/* ------------------------------------------------------------------ */
static int __init ian_list_init(void)
{
	int ret, i;
	const char *seeds[] = { "alpha", "beta", "gamma" };

	ian_kobj = kobject_create_and_add("ian_list", kernel_kobj);
	if (!ian_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(ian_kobj, &add_attr.attr);
	if (ret)
		goto err_kobj;
	ret = sysfs_create_file(ian_kobj, &del_attr.attr);
	if (ret)
		goto err_add;

	/* 塞 3 個種子節點，方便你 insmod 後馬上有東西看 */
	for (i = 0; i < 3; i++)
		add_item(seeds[i]);
	print_all();

	pr_info("ian_list: loaded. echo name > /sys/kernel/ian_list/add ; echo id > .../del\n");
	return 0;

err_add:
	sysfs_remove_file(ian_kobj, &add_attr.attr);
err_kobj:
	kobject_put(ian_kobj);
	return ret;
}

static void __exit ian_list_exit(void)
{
	struct ian_item *it, *tmp;

	/* 先拔 sysfs，確保之後不會再有 add/del 進來 */
	sysfs_remove_file(ian_kobj, &add_attr.attr);
	sysfs_remove_file(ian_kobj, &del_attr.attr);
	kobject_put(ian_kobj);

	spin_lock(&ian_lock);
	/*
	 * TODO 4：用 list_for_each_entry_safe 把鏈表上「所有」節點
	 *         list_del + kfree，整條清乾淨（不然 rmmod 就洩漏記憶體 → 第③條）。
	 *         （跟 TODO 3 同招，只是不挑 id、全部清掉，不用 break。）
	 */
	 list_for_each_entry_safe(it, tmp, &ian_list, node) {
		list_del(&it->node);
		kfree(it);
	 }
	spin_unlock(&ian_lock);

	pr_info("ian_list: unloaded\n");
}

module_init(ian_list_init);
module_exit(ian_list_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("LKD Ch6 linked-list practice: add/traverse/del with list_head");