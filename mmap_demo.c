// SPDX-License-Identifier: GPL-2.0
/*
* mmap_demo.c — mmap zero-copy 实验（影像驱动必用技术）
*
* kernel 配置一块 buffer，透过 mmap 让 user space「直接存取同一块物理记忆体」
*   → 零拷贝（不用 copy_to_user）
*   → 双向共享：user 写进去，kernel 也看得到（证明同一块物理页）
*
* 对照你最早 ian_clk.c 的 vmap 双映射（crash）：
*   那个是「cache 属性冲突」的双映射 → ARM 禁止
*   mmap 是「属性一致」的 kernel/user 双映射 → 合法、天天用
*
* 用法：
*   insmod mmap_demo.ko
*   ./mmap_reader          # user 程式 mmap 进来读/写
*   dmesg                  # 看 kernel 端是否看到 user 写的东西
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEV_NAME "mmap_demo"
#define BUF_ORDER 0
#define BUF_SIZE (PAGE_SIZE << BUF_ORDER)

struct mmap_dev {
char *buf;
dev_t devt;
struct cdev cdev;
struct class *class;
};


static int demo_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct mmap_dev *dev = filp->private_data
	unsigned long pfn;
	unsigned long size = vma->vm->end - vma->vm_start;
	
	if (size > BUF_SIZE) {
		pr_err("%s: 请求 %lu 超过 buffer %lu\n", DEV_NAME, size, (unsigned long)BUF_SIZE);
        return -EINVAL;
	}
	
	pfn = virt_to_phys(dev->buf) >> PAGE_SHIFT;
	if (remap_pfn_range(vma, vma->vm_start, pfn, size, vma->vm_page_prot))
		return -EAGAIN;
	
	pr_info("%s: mmap 成功，size=%lu，user vaddr=0x%lx\n",
              DEV_NAME, size, vma->vm_start);
    return 0;
}

static ssize_t demo_read(struct file *filp, char __user *ubuf, size_t cnt, loff_t *pos)
{
	struct mmap_dev *dev = filp->private_data;
	
	if (*pos >= BUF_SIZE)
		return 0;
	if (cnt > BUF_SIZE - *pos)
		cnt = BUF_SIZE - *pos;
	if (copy_to_user(ubuf, dev->buf + *pos, cnt))
		return -EFAULT;
	
	*pos += cnt;
	return cnt;
}

static int demo_open(struct inode *inode, struct file *filp)
{
	struct mmap_dev *dev = container_of(inode->i_cdev, struct mmap_dev, cdev);
	filp->private_data = dev;
	return 0;
}

static int demo_release(struct inode *inode, struct file *file)
{
	return 0;
}

static const struct file_operations demo_fops = {
	.owner = THIS_MODULE,
	.open = demo_open,
	.release = demo_release,
	.read = demo_read,
	.mmap = demo_mmap,
};

static int __init md_init(void)
{
	int ret;
	
	struct mmap_dev *mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	if (!mdev)
		return -ENOMEM;
	
	mdev->buf = (char *)__get_free_pages(GFP_KERNEL, BUF_ORDER);
	if (!mdev->buf) { kfree(mdev); return -ENOMEM; }
	
	snprintf(mdev->buf, BUF_SIZE, "HELLO-FROM-KERNEL: kernel vaddr=%px paddr=%pa",
			 mdev->buf, &(phys_addr_t) {virt_to_phys(mdev->buf) });
			 
	pr_info("%s: buf kernel vaddr=%px, paddr=0x%llx, size=%lu\n", DEV_NAME,
              mdev->buf, (unsigned long long)virt_to_phys(mdev->buf), (unsigned long)BUF_SIZE);		 
			 
	/* cdev 注册 */
      ret = alloc_chrdev_region(&mdev->devt, 0, 1, DEV_NAME);
      if (ret) goto err_pages;
      cdev_init(&mdev->cdev, &demo_fops);
      mdev->cdev.owner = THIS_MODULE;
      ret = cdev_add(&mdev->cdev, mdev->devt, 1);
      if (ret) goto err_region;
      mdev->class = class_create(DEV_NAME);        /* 6.12：只收 name */
      if (IS_ERR(mdev->class)) { ret = PTR_ERR(mdev->class); goto err_cdev; }
      device_create(mdev->class, NULL, mdev->devt, NULL, DEV_NAME);

      pr_info("%s: loaded, /dev/%s ready\n", DEV_NAME, DEV_NAME);
      return 0;

err_cdev:   cdev_del(&mdev->cdev);
err_region: unregister_chrdev_region(mdev->devt, 1);
err_pages:  free_pages((unsigned long)mdev->buf, BUF_ORDER);
      kfree(mdev);
      return ret;		 
	
}

static void __exit md_exit(void)
{
      /* ★ 卸载前印出 buffer 内容 → 看 user 有没有写东西进来（证明双向共享）*/
      pr_info("%s: 卸载前 buffer 内容: \"%.80s\"\n", DEV_NAME, mdev->buf);

      device_destroy(mdev->class, mdev->devt);
      class_destroy(mdev->class);
      cdev_del(&mdev->cdev);
      unregister_chrdev_region(mdev->devt, 1);
      free_pages((unsigned long)mdev->buf, BUF_ORDER);
      kfree(mdev);
      pr_info("%s: unloaded\n", DEV_NAME);
}

module_init(md_init);
module_exit(md_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("mmap zero-copy demo");