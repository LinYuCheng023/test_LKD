#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>   /* 5.10：vmalloc/vfree 要明確 include */
#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/ioctl.h>
#include <linux/pwm.h>
#include <linux/list.h>
#include <linux/kthread.h>
#include <linux/netdevice.h>
#include <linux/dma-mapping.h>
#include <linux/timer.h>

#define IAN_CLK_RATE 100000000
#define IAN_IOC_MAGIC 'i'
#define IAN_IOC_GET_RANDOM _IOR('i', 1, u32)
#define IAN_IOC_SET_SEED _IOW('i', 2, u32)
#define IAN_IOC_GET_RATE _IOR('i', 3, u32)

struct ian_node {
	u32 val;
	struct list_head list;
};

struct ian_clk {
	struct clk_hw hw;
	bool enabled;
	unsigned long rate;
	struct device *dev;
	struct clk *real_clk;
	void __iomem *rng_base;
	struct timer_list timer;
	atomic_t last_random;
	struct i2c_adapter *i2c_adap;
	dev_t devno;
	struct cdev cdev;
	struct class *cls;
	struct work_struct work;
	struct pwm_device *pwm;
	struct list_head node_list;
	int node_count;
	spinlock_t list_lock;
	struct task_struct *thread;
	void *kmalloc_buf;
	void *vmalloc_buf;
	void *dma_vaddr;
	dma_addr_t dma_paddr;
};

#define to_ian_hw(hw) container_of(hw, struct ian_clk, hw)

static int ian_clk_prepare(struct clk_hw *hw)
{
	struct ian_clk *clk = to_ian_hw(hw);
	printk(KERN_INFO "ian_clk: prepare()\n");
	 /* 真實驅動這裡等PLL鎖定喵 */
    /* usleep_range(100, 200); */
	return 0;
}

static void ian_clk_unprepare(struct clk_hw *hw)
{

	printk(KERN_INFO "ian_clk: unprepare()\n");

}

static int ian_clk_enable(struct clk_hw *hw)
{
	struct ian_clk *clk = to_ian_hw(hw);
	clk->enabled = true;
	printk(KERN_INFO "ian_clk: enable()\n");
	 /* 真實驅動這裡寫Gate暫存器喵 */
    /* writel(readl(CLK_GATE_REG) | CLK_BIT, CLK_GATE_REG); */
	return 0;
}

static void ian_clk_disable(struct clk_hw *hw)
{
	struct ian_clk *clk = to_ian_hw(hw);
	clk->enabled = false;
	printk(KERN_INFO "ian_clk: disable()\n");
	 /* 真實驅動這裡清Gate暫存器喵 */
    /* writel(readl(CLK_GATE_REG) & ~CLK_BIT, CLK_GATE_REG); */

}

static int ian_clk_is_enable(struct clk_hw *hw)
{
	struct ian_clk *clk = to_ian_hw(hw);
	return clk->enabled ? 1 : 0;
}

static unsigned long ian_clk_recalc_rate(struct clk_hw *hw,
										 unsigned long parent_rate)
{
 struct ian_clk *clk = to_ian_hw(hw);
 
 return clk->rate;
}

static long ian_clk_round_rate(struct clk_hw *hw,
							   unsigned long rate,
							   unsigned long *parent_rate)
{
	return IAN_CLK_RATE;
}

static int ian_clk_set_rate(struct clk_hw *hw,
							unsigned long rate,
							unsigned long parent_rate)
{
	struct ian_clk *clk = to_ian_hw(hw);
	printk(KERN_INFO "ian_clk: set_rate()喵 %lu Hz喵\n", rate);
	clk->rate = rate;
	
	return 0;
}


static const struct clk_ops ian_clk_ops = {
	.prepare	 = ian_clk_prepare,
	.unprepare	 = ian_clk_unprepare,
	.enable 	 = ian_clk_enable,
	.disable 	 = ian_clk_disable,
	.is_enabled  = ian_clk_is_enable,
	.recalc_rate = ian_clk_recalc_rate,
	.round_rate  = ian_clk_round_rate,
	.set_rate	 = ian_clk_set_rate,
};

/* =====================
 * sysfs控制喵
 * ===================== */
static ssize_t enable_store(struct device *dev,
							struct device_attribute *attr,
							const char *buf,
							size_t count)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);

	if (buf[0] == '1') {
		clk_prepare_enable(ian_clk->hw.clk);
		printk(KERN_INFO "ian_clk: sysfs開啟時鐘喵！\n");
	} else if (buf[0] == '0') {
		clk_disable_unprepare(ian_clk->hw.clk);
		printk(KERN_INFO "ian_clk: sysfs關閉時鐘喵！\n");
	}
	return count;
}

static ssize_t enable_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	return sprintf(buf, "%d\n", ian_clk->enabled ? 1 : 0);
}

static ssize_t rate_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	return sprintf(buf, "%lu Hz\n", ian_clk->rate);
}

static ssize_t random_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	return sprintf(buf, "0x%08x\n", (u32)atomic_read(&ian_clk->last_random));
}

static ssize_t random_store(struct device *dev,
							struct device_attribute *attr,
							const char *buf,
							size_t count)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	u32 seed;
	
	if(kstrtou32(buf, 0 , &seed))
		return -EINVAL;
	
	writel(seed, ian_clk->rng_base + 0x04);
	printk(KERN_INFO "ian_clk: Setting seed = 0x%08x\n",seed);
	return count;
}

static void ian_timer_callback(struct timer_list *t)
{
	struct ian_clk *ian_clk = container_of(t, struct ian_clk, timer);
	struct ian_node *node, *tmp;
	
	spin_lock(&ian_clk->list_lock);
	list_for_each_entry_safe(node, tmp, &ian_clk->node_list, list) {
          list_del(&node->list);
          kfree(node);   // ← free 掉 sysfs 正在用的記憶體！
      }

      /* 新增一個 */
      node = kmalloc(sizeof(*node), GFP_ATOMIC);
      if (node) {
          node->val = readl(ian_clk->rng_base + 0x0C);
          list_add(&node->list, &ian_clk->node_list);
      }
	 spin_unlock(&ian_clk->list_lock);
	//printk("timer: %s, PID=%d\n", current->comm, current->pid);
	
	mod_timer(&ian_clk->timer, jiffies + HZ/10);
}

static ssize_t nodelist_show(struct device *dev,
							 struct device_attribute *attr,
							 char *buf)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	struct ian_node *node;
	int count = 0;
	u32 last_val = 0;

	spin_lock_bh(&ian_clk->list_lock);
	list_for_each_entry_reverse(node, &ian_clk->node_list, list) {
		last_val = node->val;
		count++;
	}
	spin_unlock_bh(&ian_clk->list_lock);
	
	return sprintf(buf, "count=%d, last=0x%08x\n",count,last_val);
}

static ssize_t duty_ns_store(struct device *dev,
							 struct device_attribute *attr,
							 const char *buf,
							 size_t count)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	u32 duty_ns;

	if(kstrtou32(buf, 0 ,&duty_ns))
	 return -EINVAL;

	pwm_config(ian_clk->pwm, duty_ns, 1000);
	printk(KERN_INFO "ian_clk: duty_ns = %u\n",duty_ns);
	return count;
}

static ssize_t dma_show(struct device *dev,
						   struct device_attribute *attr,
						   char *buf)
{
	struct ian_clk *ian_clk = dev_get_drvdata(dev);
	return sprintf(buf, "dma buf[0] = 0x%08x\n", *(((u32 *)ian_clk->dma_vaddr)+1));
}

static DEVICE_ATTR(enable, 0644, enable_show ,enable_store);
static DEVICE_ATTR(rate, 0444, rate_show ,NULL);
static DEVICE_ATTR(random, 0644, random_show, random_store);
static DEVICE_ATTR(duty_ns, 0200, NULL, duty_ns_store);
static DEVICE_ATTR(nodelist, 0400, nodelist_show, NULL);
static DEVICE_ATTR(dma, 0400, dma_show, NULL);

static int ian_open(struct inode *inode, struct file *file)
{
	struct ian_clk *ian_clk = container_of(inode->i_cdev, struct ian_clk, cdev);
	file->private_data = ian_clk;
	return 0;
}

static int ian_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t ian_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct ian_clk *ian_clk = file->private_data;
	char tmp[12];
	int len;
	u32 val;
	
	if(*pos > 0)
		return 0;
	
	val = readl(ian_clk->rng_base + 0x0C);
	len = snprintf(tmp, sizeof(tmp), "0x%08x\n",val);
	
	if(copy_to_user(buf, tmp, len))
		return -EFAULT;
	
	*pos += len;
	return len;
}

static ssize_t ian_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct ian_clk *ian_clk = file->private_data;
	char tmp[12];
	int len;
	u32 seed;
	
	if (copy_from_user(tmp, buf, min(count, sizeof(tmp) - 1)))
		return -EFAULT;
	
	tmp[min(count, sizeof(tmp) - 1)] = '\0';
	
	if(kstrtou32(tmp, 0, &seed))
		return -EINVAL;
	
	writel(seed, ian_clk->rng_base + 0x04);
	printk(KERN_INFO "ian_clk: /dev/ian seed = 0x%08x\n",seed);
	
	return count;
}

static long ian_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct ian_clk *ian_clk = file->private_data;
	u32 val;
	
	switch(cmd) {
	case IAN_IOC_GET_RANDOM:
		val = readl(ian_clk->rng_base + 0x0C);
		if(copy_to_user((u32 __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
		
	case IAN_IOC_SET_SEED:
		if(copy_from_user(&val, (u32 __user *)arg, sizeof(val)))
			return -EFAULT;
		writel(val, ian_clk->rng_base + 0x04);
		break;
	
	case IAN_IOC_GET_RATE:
		val = (u32)clk_get_rate(ian_clk->real_clk);
		if(copy_to_user((u32 __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		break;
	
	default:
		return -ENOTTY;
	}
	return 0;
}

static int ian_thread_fn(void *data)
{
	struct ian_clk *ian_clk = data;
	printk(KERN_INFO "ian_clk: thread start, PID = %d\n",current->pid);
	
	while(!kthread_should_stop()) {
		//printk(KERN_INFO "ian_clk: thread read random number 0x%08x\n",readl(ian_clk->rng_base + 0x0C));
		msleep(2000);
	}
	printk(KERN_INFO "ian_clk: thread end\n");
	return 0;
}

static int ian_netdev_notifier(struct notifier_block *nb,
							   unsigned long event, 
							   void *ptr)
{
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	switch(event) {
		case NETDEV_UP:
			printk(KERN_INFO "ian_clk: network %s UP! \n",dev->name);
			break;
		case NETDEV_DOWN:
			printk(KERN_INFO "ian_clk: network %s DOWN! \n",dev->name);
			break;
	}
	return NOTIFY_OK;
}

static u64 bench_buf(volatile u32 *buf)
{
	ktime_t t0,t1;
	u32 sum = 0;
	int i;
	
	t0 = ktime_get();
	
	for(i = 0 ;i < 1024; i++)
		buf[i] = i;
	
	wmb();
	
	for(i = 0;i < 1024; i++)
		sum += buf[i];
	
	t1 = ktime_get();
	
	return ktime_to_ns(ktime_sub(t1, t0)) + (sum & 0);
}

static int ian_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ian_clk *ian_clk = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	
	if(size > 4096)
		return -EINVAL;
	
	return dma_mmap_coherent(ian_clk->dev, vma,
						     ian_clk->dma_vaddr,
							 ian_clk->dma_paddr,
							 size);
}

static struct notifier_block ian_netdev_nb = {
	.notifier_call = ian_netdev_notifier,
};

static const struct file_operations ian_fops = {
	.owner = THIS_MODULE,
	.open = ian_open,
	.release = ian_release,
	.read = ian_read,
	.write = ian_write,
	.unlocked_ioctl = ian_ioctl,
	.mmap = ian_mmap,
};
/* =====================
 * platform_driver喵
 * ===================== */
 
 static void ian_work_handler(struct work_struct *work)
 {
	 struct ian_clk *ian_clk = container_of(work, struct ian_clk, work);
	 
	 printk(KERN_INFO "ian_clk: workqeueue start! run in process:%s, PID = %d\n",current->comm, current->pid);
	 msleep(2000);
	 printk(KERN_INFO "ian_clk: workqeueue done!\n");
 }

static int ian_clk_probe(struct platform_device *pdev)
{
	struct ian_clk *ian_clk;
	struct clk_init_data init = {};
	struct clk *clk;
	int ret;
	
	printk(KERN_INFO "ian_clk: probe()喵！\n");
	
	ian_clk = devm_kzalloc(&pdev->dev, sizeof(*ian_clk), GFP_KERNEL);
	if(!ian_clk)
		return -ENOMEM;
	
	ian_clk->rate = IAN_CLK_RATE;
	ian_clk->enabled = false ;
	ian_clk->dev = &pdev->dev ;
	
	init.name = "ian_clk";
	init.ops = &ian_clk_ops;
	init.flags = 0;
	init.num_parents = 0;
	
	ian_clk->hw.init = &init;
	
	clk = clk_register(&pdev->dev, &ian_clk->hw);
	if(IS_ERR(clk)) {
		printk(KERN_ERR "ian_clk: 註冊失敗喵！\n");
        return PTR_ERR(clk);
	}
	
	clk_register_clkdev(clk, "ian_clk", NULL);
	
	//g_ian_clk = ian_clk;
	platform_set_drvdata(pdev, ian_clk);
	
	device_create_file(&pdev->dev, &dev_attr_enable);
	device_create_file(&pdev->dev, &dev_attr_rate);
	device_create_file(&pdev->dev, &dev_attr_random);
	
	printk(KERN_INFO "ian_clk: 註冊成功喵！100MHz喵\n");
    printk(KERN_INFO "ian_clk: 查看喵：\n");
    printk(KERN_INFO "cat /sys/kernel/debug/clk/ian_clk/clk_rate\n");
    printk(KERN_INFO "cat /sys/kernel/debug/clk/ian_clk/clk_enable_count\n");
    printk(KERN_INFO "echo 1 > /sys/devices/platform/ian_clk_hw/enable\n");
	
	
	
	
	ian_clk->real_clk = clk_get(NULL, "f0680000.rng");
	clk_prepare_enable(ian_clk->real_clk);
	printk(KERN_INFO "rate = %lu\n", clk_get_rate(ian_clk->real_clk));
	
	ian_clk->rng_base = ioremap(0xf0680000, 0x1000);

	/* 初始化 RNG：啟動 CASR 和 POST 處理 */
	u32 ctrl = readl(ian_clk->rng_base + 0x00);
	writel(ctrl | 0x100 | 0x200, ian_clk->rng_base + 0x00);

	u32 val = readl(ian_clk->rng_base + 0x0C);
	printk(KERN_INFO "亂數: 0x%08x\n", val);
	
	timer_setup(&ian_clk->timer, ian_timer_callback, 0);
	mod_timer(&ian_clk->timer, jiffies + HZ);
	
	ian_clk->i2c_adap = i2c_get_adapter(1);
	
	u8 reg_addr[2] = {0x30, 0x0A};
	u8 data[1] = {0};
	
	struct i2c_msg msgs[2] = {
		{.addr = 0x1a, .flags = 0, .len = 2, .buf = reg_addr },
		{.addr = 0x1a, .flags = I2C_M_RD, .len = 1, .buf = data },
	};
	
	ret = i2c_transfer(ian_clk->i2c_adap, msgs, 2);
	printk(KERN_INFO "IMX307 reg 0x3013 = 0x%02x, ret = %d\n",data[0],ret);
	
	/*u8 buf[3] = {0x30, 0x00, 0x01};
	
	struct i2c_msg msg = {
		.addr = 0x1a, 
		.flags = 0,
		.len = 3,
		.buf = buf,
	};
	
	ret = i2c_transfer(ian_clk->i2c_adap, &msg, 1);
	printk(KERN_INFO "Write ret = %d\n",ret);
	
	buf[0] = 0x30; buf[1] = 0x02; buf[2] = 0x01;
	ret = i2c_transfer(ian_clk->i2c_adap, &msg, 1);
	printk(KERN_INFO "Write ret = %d\n",ret);
	
	msleep(3000);
	
	buf[0] = 0x30; buf[1] = 0x00; buf[2] = 0x00;
	i2c_transfer(ian_clk->i2c_adap, &msg, 1);

	buf[0] = 0x30; buf[1] = 0x02; buf[2] = 0x00;
	i2c_transfer(ian_clk->i2c_adap, &msg, 1);

	printk(KERN_INFO "sensor wakeup\n");*/
	
	alloc_chrdev_region(&ian_clk->devno, 0, 1, "ian");
	cdev_init(&ian_clk->cdev, &ian_fops);
	cdev_add(&ian_clk->cdev, ian_clk->devno, 1);
	ian_clk->cls = class_create("ian");   /* 5.10：只收 name 一個參數 */
	device_create(ian_clk->cls, NULL, ian_clk->devno, NULL, "ian");
	
	
	INIT_WORK(&ian_clk->work, ian_work_handler);
	schedule_work(&ian_clk->work);
	
	
	
	ian_clk->pwm = pwm_get(&pdev->dev, "ian_pwm");
	if(!IS_ERR(ian_clk->pwm)) {
		pwm_config(ian_clk->pwm, 500, 1000);
		pwm_enable(ian_clk->pwm);
		printk(KERN_INFO "ian_clk: PWM channel 8 enabled\n");
	}
	
	device_create_file(&pdev->dev, &dev_attr_duty_ns);
	
	
	INIT_LIST_HEAD(&ian_clk->node_list);
	
	device_create_file(&pdev->dev, &dev_attr_nodelist);
	
	spin_lock_init(&ian_clk->list_lock);
 
	ian_clk->thread = kthread_run(ian_thread_fn, ian_clk, "ian_thread");
	if(IS_ERR(ian_clk->thread))
		printk(KERN_ERR "ian_clk:kthread creation fail\n");
	
	
	register_netdevice_notifier(&ian_netdev_nb);
	
	
	ian_clk->kmalloc_buf = kmalloc(SZ_1M, GFP_KERNEL);
	printk(KERN_INFO "kmalloc vaddr =  0x%lx\n",(unsigned long)ian_clk->kmalloc_buf);
	printk(KERN_INFO "kmalloc paddr =  0x%lx\n",(unsigned long)virt_to_phys(ian_clk->kmalloc_buf));
	
	device_create_file(&pdev->dev, &dev_attr_dma);
	
	ian_clk->vmalloc_buf = vmalloc(10 *1024 * 1024);
	printk(KERN_INFO "vmalloc vaddr =  0x%lx\n",(unsigned long)ian_clk->vmalloc_buf);
	
	void *addr = ian_clk->vmalloc_buf;
	int i;
	for(i = 0;i < 5 ;i++) {
		struct page *page = vmalloc_to_page(addr + i *4096);
		phys_addr_t paddr = page_to_phys(page);
		printk(KERN_INFO "vmalloc page[%d] vaddr = 0x%lx -> paddr =  0x%lx\n",
																			 i,
																			 (unsigned long)(addr + i *4096),
																			 (unsigned long)paddr);
	}
	
	
	ian_clk->dma_vaddr = dma_alloc_coherent(&pdev->dev, 4096, &ian_clk->dma_paddr, GFP_KERNEL);
	
	if(!ian_clk->dma_vaddr) {
		printk(KERN_ERR "ian_clk: dma_alloc_coherent failed\n");
	} else {
		printk(KERN_INFO "dma vaddr = 0x%lx\n",(unsigned long)ian_clk->dma_vaddr);
		printk(KERN_INFO "dma paddr = 0x%lx\n",(unsigned long)ian_clk->dma_paddr);
		
		*(u32 *)ian_clk->dma_vaddr = 0xDEADBEEF;
		printk(KERN_INFO "dma buf[0] = 0x%08x\n", *(u32 *)ian_clk->dma_vaddr);
	}
	
	/*
	for(i = 0;i < 20 ;i++)
		printk(KERN_INFO "kmalloc Execution[%d] time: %lld ns\n",i, (long long)bench_buf(ian_clk->kmalloc_buf));
	for(i = 0;i < 20 ;i++)
		printk(KERN_INFO "dma_alloc_coherent Execution[%d] time: %lld ns\n",i, (long long)bench_buf(ian_clk->dma_vaddr));
	*/
	
	size_t sizes[] = {SZ_4K, SZ_64K, SZ_1M};
	for(i = 0;i < 3; i++) {
		ktime_t t0,t1;
		memset(ian_clk->kmalloc_buf, i, sizes[i]);
		t0 = ktime_get();
		dma_addr_t h = dma_map_single(&pdev->dev, ian_clk->kmalloc_buf, sizes[i], DMA_TO_DEVICE);   
		t1 = ktime_get();
		dma_unmap_single(&pdev->dev, h, sizes[i], DMA_TO_DEVICE);
		printk(KERN_INFO "clean %zu bytes: %lld ns\n",sizes[i] , ktime_to_ns(ktime_sub(t1, t0)));
	}
	
#if 0
	/* stale data live demo — 在 NT98525 (ARMv7) 上無法執行:
	 * vmap + pgprot_noncached 對同一實體頁建 cached/uncached 雙映射,
	 * 違反 ARMv7 aliasing 規則 (unpredictable),4.19 kernel 內部 crash。
	 * ioremap 路徑則被 pfn_valid() 防呆擋掉回 NULL。
	 * 結論:「知道為什麼做不了」本身就是完整的學習收穫。 */
	{
		struct page *pg = virt_to_page(ian_clk->kmalloc_buf);
		volatile u32 *uncached = vmap(&pg, 1, VM_MAP, pgprot_noncached(PAGE_KERNEL));
		volatile u32 *cached = (volatile u32 *)ian_clk->kmalloc_buf;
		dma_addr_t h;

		if(!uncached) {
			printk(KERN_ERR "vmap 失敗,跳過 demo\n");
		} else {
			*cached = 0xAAAAAAAA;
			printk(KERN_INFO "1. cached write, read back: 0x%08x\n",*cached);

			*uncached = 0xBBBBBBBB;
			printk(KERN_INFO "2. uncached write, cached read : 0x%08x (stale?)\n",*cached);

			h = dma_map_single(&pdev->dev, ian_clk->kmalloc_buf, PAGE_SIZE, DMA_FROM_DEVICE);
			dma_unmap_single(&pdev->dev, h, PAGE_SIZE, DMA_FROM_DEVICE);

			printk(KERN_INFO "3. After invalidate, cached read: 0x%08x\n",*cached);

			vunmap((void *)uncached);
		}
	}
#endif
	
	
    return 0;
}

static void ian_clk_remove(struct platform_device *pdev)   /* 5.10：remove 回 void */
{
	struct ian_clk *ian_clk = platform_get_drvdata(pdev);
	i2c_put_adapter(ian_clk->i2c_adap);
	device_remove_file(&pdev->dev, &dev_attr_enable);
	device_remove_file(&pdev->dev, &dev_attr_rate);
	device_remove_file(&pdev->dev, &dev_attr_random);
	device_remove_file(&pdev->dev, &dev_attr_duty_ns);
	device_remove_file(&pdev->dev, &dev_attr_nodelist);
	device_remove_file(&pdev->dev, &dev_attr_dma);
	iounmap(ian_clk->rng_base);
	timer_delete_sync(&ian_clk->timer);
	clk_disable_unprepare(ian_clk->real_clk);
	clk_put(ian_clk->real_clk);
	clk_unregister(ian_clk->hw.clk);
	if(!IS_ERR(ian_clk->pwm)) {
		pwm_disable(ian_clk->pwm);
		pwm_put(ian_clk->pwm);
	}
	
	if(ian_clk->dma_vaddr) 
		dma_free_coherent(&pdev->dev, 4096, ian_clk->dma_vaddr, ian_clk->dma_paddr);
	
	kfree(ian_clk->kmalloc_buf);
	vfree(ian_clk->vmalloc_buf);
	device_destroy(ian_clk->cls, ian_clk->devno);
	class_destroy(ian_clk->cls);
	cdev_del(&ian_clk->cdev);
	unregister_chrdev_region(ian_clk->devno, 1);
	cancel_work_sync(&ian_clk->work);
	kthread_stop(ian_clk->thread);
	unregister_netdevice_notifier(&ian_netdev_nb);
	printk(KERN_INFO "ian_clk: 移除喵！\n");
}

static const struct of_device_id ian_clk_of_match[] = {
	{ .compatible = "ian,clk" },
	{ }
};

MODULE_DEVICE_TABLE(of, ian_clk_of_match);

static struct platform_driver ian_clk_driver = {
	.driver = {
		.name = "ian_clk_hw",
		.of_match_table = ian_clk_of_match,
	},
	.probe = ian_clk_probe,
	.remove = ian_clk_remove,
};

module_platform_driver(ian_clk_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ian");
MODULE_DESCRIPTION("Virtual Clock Driver Practice");
MODULE_VERSION("V1.0");