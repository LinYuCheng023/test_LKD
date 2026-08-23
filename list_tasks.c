#include <linux/module.h>
#include <linux/sched/signal.h>


static int __init list_tasks_init(void)
{
	struct task_struct *task;
	
	for_each_process(task)
		printk(KERN_INFO "%s [%d]\n",task->comm, task->pid);
		
	return 0;
}

static void __exit list_tasks_exit(void)
{
}

module_init(list_tasks_init);
module_exit(list_tasks_exit);
MODULE_LICENSE("GPL");