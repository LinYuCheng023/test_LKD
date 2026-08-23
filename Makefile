CROSS_COMPILE ?= aarch64-linux-gnu-
ARCH          ?= arm64
KERNEL_DIR    ?= /lib/modules/$(shell uname -r)/build
CC := gcc

# 核心模組清單(只需維護這一份)
obj-m += preempt_scope.o
obj-m += ian_clk.o
obj-m += list_tasks.o
obj-m += kthread_demo.o
obj-m += list_demo.o
obj-m += fifo_demo.o
obj-m += idr_demo.o
obj-m += rb_demo.o
obj-m += crash_interrupt.o
obj-m += gpio_irq_demo.o
obj-m += gpio_irq_threaded.o
obj-m += gpio_irq_tasklet.o
obj-m += comp_demo.o
obj-m += mutex_demo.o
obj-m += mutex_atomic.o
obj-m += spin_deadlock.o
obj-m += key_debounce.o
obj-m += sensor_sample.o
obj-m += sensor_sample_todo.o
obj-m += mem_barrier.o
obj-m += abba_deadlock.o
obj-m += false_sharing.o
obj-m += tear_smp.o
obj-m += tear_crash.o


# 使用者空間測試程式清單
USER_PROGS := test_ioctl test_mmap test_lazy test_null test_tlb test_dma test_container sensor_reader sensor_poll_reader sensor_ioctl

ccflags-y += -g
#ccflags-y += -Werror

.PHONY: all modules clean

all: modules $(USER_PROGS) 

modules:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

# Pattern rule 取代原本 7 個重複的規則
$(USER_PROGS): %: %.c
	$(CC) -O2 -Wall $< -o $@

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f $(USER_PROGS)
