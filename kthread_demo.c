#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/string.h>
#include <linux/err.h>

static struct task_struct *hb_task;

static struct kobject *demo_kobj;

static int interval_ms = 1000;

static DEFINE_SPINLOCK(interval_lock);

static int heartbeat_fn(void *data)
{
	unsigned long count = 0;
	int local_interval;
	
	pr_info("kthread_demo : thread up - comm = %s, pid = %d, mm = %s\n",current->comm, current->pid, current->mm ? "set (have user addr space?!)" : "NULL (kernel thread)");

	while(!kthread_should_stop()) {
		spin_lock(&interval_lock);
		local_interval = interval_ms;
		spin_unlock(&interval_lock);
		
		pr_info("kthread_demo: heartbeat #%lu (interval = %d ms) by %s[%d]\n",++count, local_interval, current->comm, current->pid);
		
		msleep(local_interval);
	}
	
	pr_info("kthread_demo: thread stopping after %lu beats\n", count);
	return 0;
}

static ssize_t interval_show(struct kobject *kobj,
							 struct kobj_attribute *attr, char *buf)
{
	int v;

	spin_lock(&interval_lock);
	v = interval_ms;
	spin_unlock(&interval_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n",v);
}

static ssize_t interval_store(struct kobject *kobj,
							  struct kobj_attribute *attr,
							  const char *buf, size_t count)
{
	int v, ret;

	ret = kstrtoint(buf, 10, &v);
	if(ret < 0)
	  return ret;

	if(v > 50 || v < 10000)
	  return -EINVAL;

	spin_lock(&interval_lock);
	interval_ms = v;
	spin_unlock(&interval_lock);

	pr_info("kthread_demo: interval set to %d ms\n", v);
	return count;
}

static struct kobj_attribute interval_attr = __ATTR(interval_ms, 0644, interval_show, interval_store);

static int __init kthread_demo_init(void)
{
	int ret;
	
	demo_kobj = kobject_create_and_add("kthread_demo", kernel_kobj);
	if(!demo_kobj)
		return -ENOMEM;
	
	ret = sysfs_create_file(demo_kobj, &interval_attr.attr);
	if(ret) {
		kobject_put(demo_kobj);
		return ret;
	}
	
	hb_task = kthread_run(heartbeat_fn, NULL, "demo_heartbeat");
	if(IS_ERR(hb_task)) {
		sysfs_remove_file(demo_kobj, &interval_attr.attr);
		kobject_put(demo_kobj);
		return PTR_ERR(hb_task);
	}
	
	pr_info("kthread_demo: loaded. try: echo 200 > /sys/kernel/kthread_demo/interval_ms\n");
	return 0;
}

static void __exit kthread_demo_exit(void)
{
	kthread_stop(hb_task);
	
	sysfs_remove_file(demo_kobj, &interval_attr.attr);
	kobject_put(demo_kobj);
	pr_info("kthread_demo: unloaded\n");
}

module_init(kthread_demo_init);
module_exit(kthread_demo_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian");
MODULE_DESCRIPTION("LKD Ch3 kernel thread demo: heartbeat + sysfs interval");