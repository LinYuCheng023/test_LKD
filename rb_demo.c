// SPDX-License-Identifier: GPL-2.0
/*
 * rb_demo.c — CH6 §6.4 二叉樹（rbtree / 紅黑樹）實戰（骨架 + TODO）
 *
 * rbtree = 保證 O(log n) 的「有序」容器。特色：search/insert 要你自己寫
 *          比較邏輯（C 沒泛型），rbtree 只幫你扛「再平衡」(rb_insert_color)。
 *
 * 這個 demo：用 key 插入 (key,name)、用 key 查找、用 key 刪除、有序印出全部。
 *   插入：echo "5 alpha" > /sys/kernel/rb_demo/insert   （key=5, name=alpha）
 *   查找：echo 5         > /sys/kernel/rb_demo/find
 *   刪除：echo 5         > /sys/kernel/rb_demo/remove
 *   印出：cat             /sys/kernel/rb_demo/dump       （★會依 key 排序★）
 *
 * 4 個 TODO（cleanup 我幫你填好了當參考）：
 *   TODO 1：rb_insert  —— 找位置 + rb_link_node + rb_insert_color（核心，最難）
 *   TODO 2：rb_search  —— 比較往左/往右的搜尋迴圈
 *   TODO 3：rb_erase   —— 從樹移除（只移除不 kfree！）
 *   TODO 4：dump       —— rb_first + rb_next 有序走訪
 *
 * 提示：rb_entry(ptr, type, member) 就是 container_of，用來從 rb_node 反推外層 my_node。
 *
 * 目標平台：Linux 5.4.x（NT98525 / QEMU vexpress-a9）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/rbtree.h>    /* 本節主角 */
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/string.h>

/* 每個節點：rb_node「嵌」進資料 struct（跟 list_head 同一個侵入式套路）*/
struct my_node {
	struct rb_node node;
	unsigned long  key;      /* 用來排序/查找的鍵 */
	char           name[32];
};

static struct rb_root demo_root = RB_ROOT;   /* 樹根，初始化成空樹 */
static DEFINE_SPINLOCK(demo_lock);           /* 保護樹（第⑥條）；操作不睡故用 spinlock */
static struct kobject *rb_kobj;

/* ------------------------------------------------------------------ */
/* 搜尋：給 key，回傳對應 my_node*（找不到回 NULL）。呼叫者負責上鎖。    */
/* ------------------------------------------------------------------ */
static struct my_node *rb_search(unsigned long key)
{
	struct rb_node *n = demo_root.rb_node;   /* 從樹根開始 */

	/*
	 * TODO 2：寫搜尋迴圈。
	 *   while (n) {
	 *       struct my_node *cur = rb_entry(n, struct my_node, node);
	 *       if (key < cur->key)       n = n->rb_left;   // 比較小 → 往左
	 *       else if (key > cur->key)  n = n->rb_right;  // 比較大 → 往右
	 *       else                      return cur;       // 相等 → 找到
	 *   }
	 *   （那三個 if/else 就是你自己填的「比較邏輯」，BST 有序性靠它）
	 */
	 while(n) {
		 struct my_node *cur = rb_entry(n, struct my_node, node);
		 if(key < cur->key)
			 n = n->rb_left;
		 else if(key > cur->key)
			 n = n->rb_right;
		 else
			 return cur;
	 }

	return NULL;   /* 走到底沒找到 */
}

/* ------------------------------------------------------------------ */
/* 插入：把 new 插進樹，成功回 0、key 已存在回 -EEXIST。呼叫者負責上鎖。 */
/* ------------------------------------------------------------------ */
static int rb_insert(struct my_node *new)
{
	struct rb_node **p = &demo_root.rb_node;   /* 指標的指標：指向「要填的那個空位」*/
	struct rb_node *parent = NULL;
	struct my_node *cur;

	/*
	 * TODO 1：先像搜尋一樣走到該插入的空位，途中記住 parent，
	 *         最後用 rb_link_node + rb_insert_color 掛上並再平衡。
	 *
	 *   while (*p) {
	 *       parent = *p;
	 *       cur = rb_entry(parent, struct my_node, node);
	 *       if (new->key < cur->key)       p = &(*p)->rb_left;
	 *       else if (new->key > cur->key)  p = &(*p)->rb_right;
	 *       else                           return -EEXIST;   // key 重複
	 *   }
	 *   rb_link_node(&new->node, parent, p);          // ① 把新節點掛到空位
	 *   rb_insert_color(&new->node, &demo_root);      // ② 執行紅黑再平衡（旋轉/變色都在這）
	 */
	 
	 while(*p) {
		parent = *p;
		cur = rb_entry(parent, struct my_node, node);
		if(new->key < cur->key)
			p = &(*p)->rb_left;
		else if(new->key > cur->key)
			p = &(*p)->rb_right;
		else
			return -EEXIST;
	 }
	 rb_link_node(&new->node, parent, p);
	 rb_insert_color(&new->node, &demo_root);

	return 0;
}

/* ------------------------------------------------------------------ */
/* sysfs: echo "key name" > insert                                     */
/* ------------------------------------------------------------------ */
static ssize_t insert_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct my_node *item;
	unsigned long key;
	char name[32];
	int ret;

	name[0] = '\0';
	ret = sscanf(buf, "%lu %31s", &key, name);   /* 解析 "key name" */
	if (ret < 1)
		return -EINVAL;

	item = kmalloc(sizeof(*item), GFP_KERNEL);   /* 鎖外配（GFP_KERNEL 可能睡，第⑤條）*/
	if (!item)
		return -ENOMEM;
	item->key = key;
	strscpy(item->name, name, sizeof(item->name));

	spin_lock(&demo_lock);
	ret = rb_insert(item);
	spin_unlock(&demo_lock);

	if (ret) {                    /* -EEXIST：key 重複，沒插進去 */
		kfree(item);          /* 沒用到的 item 要還回去（第③條）*/
		pr_info("rb_demo: key=%lu already exists\n", key);
		return ret;
	}

	pr_info("rb_demo: inserted key=%lu name=%s\n", key, item->name);
	return count;
}

/* ------------------------------------------------------------------ */
/* sysfs: echo key > find                                              */
/* ------------------------------------------------------------------ */
static ssize_t find_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct my_node *item;
	unsigned long key;

	if (kstrtoul(buf, 10, &key))
		return -EINVAL;

	spin_lock(&demo_lock);
	item = rb_search(key);        /* 用你 TODO 2 寫的搜尋 */
	spin_unlock(&demo_lock);

	if (item)
		pr_info("rb_demo: found key=%lu name=%s\n", key, item->name);
	else
		pr_info("rb_demo: key=%lu not found\n", key);
	return count;
}

/* ------------------------------------------------------------------ */
/* sysfs: echo key > remove                                            */
/* ------------------------------------------------------------------ */
static ssize_t remove_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct my_node *item;
	unsigned long key;

	if (kstrtoul(buf, 10, &key))
		return -EINVAL;

	spin_lock(&demo_lock);
	item = rb_search(key);
	if (item) {
		/*
		 * TODO 3：用 rb_erase 把節點從樹移除。
		 *   API：void rb_erase(struct rb_node *node, struct rb_root *root)
		 *        node = &item->node, root = &demo_root
		 *   注意：rb_erase 只從樹移除、不 kfree（跟 list_del / idr_remove 一樣的坑）→
		 *         移除後要自己 kfree（下面已幫你寫）。
		 */
		/* TODO 3：填這裡 */
		rb_erase(&item->node, &demo_root);
	}
	spin_unlock(&demo_lock);

	if (!item) {
		pr_info("rb_demo: key=%lu not found\n", key);
		return -ENOENT;
	}

	kfree(item);   /* 已從樹移除，沒人拿得到了 → 出鎖後 kfree 安全 */
	pr_info("rb_demo: removed key=%lu\n", key);
	return count;
}

/* ------------------------------------------------------------------ */
/* sysfs: cat dump —— 有序印出（rbtree 的招牌：走出來是排序好的）        */
/* ------------------------------------------------------------------ */
static ssize_t dump_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	int len = 0;

	spin_lock(&demo_lock);
	/*
	 * TODO 4：用 rb_first + rb_next 有序走訪整棵樹，把每個 key/name 累加進 buf。
	 *
	 *   struct rb_node *n;
	 *   for (n = rb_first(&demo_root); n; n = rb_next(n)) {
	 *       struct my_node *item = rb_entry(n, struct my_node, node);
	 *       len += scnprintf(buf + len, PAGE_SIZE - len,
	 *                        "key=%lu name=%s\n", item->key, item->name);
	 *   }
	 *   （rb_first = 最小的 key，rb_next = 下一個更大的 → 走出來就是排序後的順序）
	 */
	 struct rb_node *n;
	 for(n = rb_first(&demo_root);n; n = rb_next(n)) {
		struct my_node *item = rb_entry(n, struct my_node, node);
		len += scnprintf(buf + len, PAGE_SIZE - len, "key = %lu, name = %s\n",item->key, item->name);
	 }
	spin_unlock(&demo_lock);

	if (len == 0)
		len = scnprintf(buf, PAGE_SIZE, "(empty)\n");
	return len;
}

static struct kobj_attribute insert_attr = __ATTR(insert, 0220, NULL,      insert_store);
static struct kobj_attribute find_attr   = __ATTR(find,   0220, NULL,      find_store);
static struct kobj_attribute remove_attr = __ATTR(remove, 0220, NULL,      remove_store);
static struct kobj_attribute dump_attr   = __ATTR(dump,   0440, dump_show, NULL);

/* ------------------------------------------------------------------ */
static int __init rb_demo_init(void)
{
	int ret;

	rb_kobj = kobject_create_and_add("rb_demo", kernel_kobj);
	if (!rb_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(rb_kobj, &insert_attr.attr);
	if (ret)
		goto err_kobj;
	ret = sysfs_create_file(rb_kobj, &find_attr.attr);
	if (ret)
		goto err_insert;
	ret = sysfs_create_file(rb_kobj, &remove_attr.attr);
	if (ret)
		goto err_find;
	ret = sysfs_create_file(rb_kobj, &dump_attr.attr);
	if (ret)
		goto err_remove;

	pr_info("rb_demo: loaded. echo \"key name\">insert ; echo key>find/remove ; cat dump\n");
	return 0;

err_remove:
	sysfs_remove_file(rb_kobj, &remove_attr.attr);
err_find:
	sysfs_remove_file(rb_kobj, &find_attr.attr);
err_insert:
	sysfs_remove_file(rb_kobj, &insert_attr.attr);
err_kobj:
	kobject_put(rb_kobj);
	return ret;
}

static void __exit rb_demo_exit(void)
{
	struct my_node *item, *tmp;

	/* 先拔 sysfs（反向拆解）*/
	sysfs_remove_file(rb_kobj, &insert_attr.attr);
	sysfs_remove_file(rb_kobj, &find_attr.attr);
	sysfs_remove_file(rb_kobj, &remove_attr.attr);
	sysfs_remove_file(rb_kobj, &dump_attr.attr);
	kobject_put(rb_kobj);

	spin_lock(&demo_lock);
	/*
	 * 清空整棵樹（這段我幫你填好，當參考）：
	 * rbtree_postorder_for_each_entry_safe 是「後序走訪」——先處理子節點再處理父節點，
	 * 所以走訪中 kfree 節點很安全（不會踩到已釋放的父/子）。
	 * 這是清空 rbtree 的標準慣例，不需要一個個 rb_erase。
	 */
	rbtree_postorder_for_each_entry_safe(item, tmp, &demo_root, node) {
		kfree(item);
	}
	demo_root = RB_ROOT;   /* 樹重設成空 */
	spin_unlock(&demo_lock);

	pr_info("rb_demo: unloaded\n");
}

module_init(rb_demo_init);
module_exit(rb_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("LKD Ch6 rbtree demo: insert/find/erase/ordered-dump");