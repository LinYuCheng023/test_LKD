# Linux Kernel 中斷與同步筆記（CH7–CH10）

> 平台：NT98525 / ARM 32-bit **單核** / Linux **4.19.91**（書為 2.6，差異已標註）
> 方法：讀書 → 對照 tree 實碼 → 板子實測 → 對照原廠 Novatek 驅動
> 實驗檔：`gpio_irq_threaded.c`(A2/A1/C1/B1) `gpio_irq_tasklet.c`(A3) `preempt_scope.c`(D1) `comp_demo.c`(D2) `mutex_demo.c`(D3) `mutex_atomic.c`(D4) `spin_deadlock.c`(D5)

---

## 一、核心觀念總表（全部板子驗證過）

### 三種上下文 vs 能不能睡（`in_interrupt()` 實測值）
| 上下文 | preempt_count | in_interrupt() | 能睡 | 出現在 |
|---|---|---|---|---|
| 上半部 ISR | 0x10000 (HARDIRQ) | 65536 | ❌ | A3 |
| tasklet/softirq | 0x100 (SOFTIRQ) | 256 | ❌ | A3/D4 |
| process(kthread/workqueue) | 0 | 0 | ✅ | A2/D2/D3 |

### preempt_count 位元佈局（D1 全部點亮）
```
0x000f0000 HARDIRQ | 0x0000ff00 SOFTIRQ | 0x000000ff PREEMPT
```
- `in_interrupt()` = HARDIRQ|SOFTIRQ 欄（**不含 PREEMPT**）→ 判斷「在不在中斷上下文/能不能睡」
- 關搶佔 → PREEMPT 欄（軟體計數器）；關中斷 → **CPSR I bit**（bit7=0x80=128，`irqs_disabled()` 才看得到，不在 preempt_count 裡）
- 「關搶佔」和「關中斷」是**兩套獨立系統**（D1 實測：preempt_disable 只 PREEMPT=1；spin_lock_irqsave 是 PREEMPT=1 + irqs_disabled=128）

---

## 二、CH7 中斷

### 中斷路徑（ARM 4.19，非書的 x86）
```
硬體 IRQ → entry-armv.S vector_irq/__irq_svc(存現場)
  → ldr pc,[handle_arch_irq] → gic_handle_irq(讀IAR ack+取號 / 寫EOI收尾)
  → __handle_domain_irq → irq_find_mapping(radix tree/linear) → generic_handle_irq
  → handle_irq_event → for_each_action_of_desc(共享鏈表) → 你的 ISR
  → irq_exit(跑下半部) → 檢查搶佔 → svc_exit(復原現場)
```
- **GIC 握手**：讀 IAR = ack+取號+自動撤線；寫 EOI = 告訴 GIC 處理完（少寫 → 中斷只來一次）
- **hwirq→virq 映射**：GPIO129 → GPIO domain hwirq 33 →(irq_domain 反查表)→ 全域 virq 289
  - 反查表兩種：`linear_revmap[]`陣列(hwirq少而密,GPIO用這種) / `radix_tree`(稀疏/大)
  - `/proc/interrupts` 的計數 = **上半部(hardirq)次數**，非下半部

### 書過時處
- `IRQF_DISABLED` → 2.6.35 移除；4.19 一律「ISR 執行時關本地中斷」，無最好/最壞之分
- `do_IRQ`/舊框架 → 已被 generic irq framework 取代

---

## 三、CH8 下半部

### 三種下半部（+ threaded IRQ）
| 機制 | context | 能睡 | 序列化 | 實作 |
|---|---|---|---|---|
| softirq | 中斷 | ❌ | 無(同種可多核並行→自己上鎖) | — |
| tasklet | 中斷(softirq) | ❌ | 同種不並行(RUN bit,單核no-op) | A3 |
| workqueue | process | ✅ | 無 | 最初版 |
| threaded IRQ(書沒有) | process | ✅ | — | A2 |

**決策**：要睡→workqueue；不睡→tasklet；要壓榨多核→softirq(網路/block)

### 關鍵機制
- **合併**：tasklet 靠 `TASKLET_STATE_SCHED` bit、threaded 靠 `IRQTF_RUNTHREAD` bit → 多次觸發合併成少次（A1: top>bottom）。**completion 相反，計數不合併**（D2）
- **softirq 索引=優先權**（interrupt.h）：`HI(0) TIMER(1) NET_TX(2) NET_RX(3) BLOCK(4) IRQ_POLL(5) TASKLET(6) SCHED(7) HRTIMER(8,停用) RCU(9)`。A3 的 `softirq 6 TASKLET` 對上了。書表 8-2 索引不同(4.19 多了 IRQ_POLL)
- **ksoftirqd**：softirq 爆量(irq_exit 的 2ms/10輪 預算用完)時 wakeup；nice-19 後備 kthread；跑的是**同一個 `__do_softirq`**。用 smpboot 框架(非書的 for(;;))
- **irq_exit** = 下半部執行 + 搶佔檢查的共同觸發點

### `/proc/softirqs` 實測（這顆 AV SoC）
`TASKLET 7000萬(冠軍)` → 影像 pipeline(SIE/IFE/IPE/DCE/VPE/JPEG/codec)全用 tasklet；`SCHED=0` → 單核鐵證

### 書過時處：BH、task queue 已於 2.5 移除（純歷史）

---

## 四、CH9 同步理論

- **臨界區/競態/同步** = B1 撕裂的正式命名
- **並發五來源**：中斷/下半部/內核搶佔/睡眠/SMP。**單核只少 SMP，前四個照樣咬人 → 單核≠安全**（B1 實證）
- **`i++` 非原子**（讀-改-寫三步）→ 計數器要用 `atomic_t`
- **★ 鎖資料，不是鎖 code**：鎖綁資料，所有碰它的地方拿同一把（B1 `rec_lock`、VPE `linklist_lock`）。BKL 反面教材(鎖 code→無法拆細→被廢)
- **死鎖**：自死鎖(Linux 無遞迴鎖)、ABBA(反序拿)。頭號規則：**固定加鎖順序**
- **鎖粒度**：粗(一把管大塊)vs 細(一把管小塊)。**單核用粗鎖才對**(細鎖是給多核解爭用的，單核純增開銷)→ B1/VPE 單鎖正確

---

## 五、CH10 同步方法（工具箱）

### 決策表（整章高潮，全部實測）
| 需求 | 用 | 原因 |
|---|---|---|
| 單一變數 | atomic_t | 硬體原子(ARM ldrex/strex) |
| 短期/中斷上下文 | spinlock | 忙等,不睡 |
| 長期/要睡 | mutex | 睡眠,讓CPU |
| 中斷上下文 | **只能 spinlock** | 中斷不能睡 |
| 要睡 | **只能 mutex** | 原子上下文禁睡 |

### spinlock（B1）
- 忙等待，持有必須極短
- 家族：`spin_lock`(關搶佔) / `_bh`(+關下半部) / `_irqsave`(+關中斷,存還原狀態)
- **選哪個看對方在哪層**：process→lock、tasklet→_bh、中斷→irqsave。關到剛好夠(irqsave關中斷傷即時性,能用_bh就別用)
- **單核上 spinlock ≈ 只剩關搶佔**；B1 擋撕裂靠的是 irqsave 的「關中斷」(irqs_disabled=128)，非自旋
- ISR 用鎖必須先關中斷 → 否則 process 持鎖被 ISR 打斷、ISR 空轉等永不釋放的鎖 = 死鎖
- 不可遞迴(D5 自死鎖)

### 原子性 ≠ 順序性
- atomic/鎖 保證原子性；**順序性靠 barrier**
- **ARM 是弱順序**(x86 強順序)→ barrier 真必要，別套 x86 直覺
- `barrier()`只擋編譯器；`mb/rmb/wmb`擋編譯器+CPU；`smp_*`單核退化成barrier()
- **碰硬體/DMA 用不帶 smp 的 `wmb()`**(單核也不能退化)。鎖含隱式屏障→用鎖時不用自己加

### mutex（D3）— 現代首選睡眠鎖
- 記 owner；五規則：count=1 / **誰鎖誰放** / 不可遞迴 / 持鎖不可退出 / **不能在中斷/下半部用**
- 首選 mutex，規則擋住才用 semaphore
- `CONFIG_DEBUG_MUTEXES` 抓「非持有者解鎖」(但 `DEBUG_LOCKS_WARN_ON` 只噴一次就 debug_locks=0 靜默)

### semaphore（VPE `vos_sem_wait`）
- 睡眠鎖，可 N 持有者(計數)。4.19 API 改：`DECLARE_MUTEX`→`DEFINE_SEMAPHORE`、搬到 `<linux/semaphore.h>`
- **4.19 別拿它當互斥鎖(那是 mutex 的活)**，主要用於計數 N 持有者

### completion（VPE `complete`，D2）
- 「A 等事件、B 通知」的睡眠同步 = semaphore 簡化版
- `wait_for_completion`(等,睡) / `complete`(通知,不睡→中斷上下文可用)
- **計數不合併**(done 計數器)，每發必記(D2: wake_count 精準對上按鍵數)

### 禁搶佔（D1）
- `preempt_disable/enable`(巢狀計數,操作 PREEMPT 欄)；spinlock 當非搶佔標記
- per-CPU 資料不用鎖但要關搶佔(防搶佔造成的偽並行)；`get_cpu/put_cpu`

### 禁下半部
- `local_bh_disable/enable`(操作 SOFTIRQ 欄,巢狀計數)；管不到 workqueue(process context)
- `spin_lock_bh` = local_bh_disable + spin_lock

### 書過時處：BKL(2.6.39 移除,鎖 code 反面教材)

---

## 六、實驗結論速查（A/B/C/D）

| 實驗 | 檔案 | 結論 |
|---|---|---|
| **A2** threaded IRQ | gpio_irq_threaded.c | 專屬 kthread(pid固定)、下半部 in_interrupt=0 可睡、IRQ_WAKE_THREAD |
| **A1** 下半部合併 | 同上 | 拿掉 ONESHOT+彈跳 → top>bottom(IRQTF_RUNTHREAD bit 合併) |
| **C1** 映射驗證 | 同上 | /proc/interrupts=上半部次數；hwirq33→virq289；chip=f0070000.gpio |
| **B1** spin_lock_irqsave | 同上 | 多欄位{a,b,c}撕裂(a=舊 b=c=新)→加鎖歸零；靠關中斷擋 ISR；臨界區短、printk 放鎖外 |
| **D1** preempt_count 顯微鏡 | preempt_scope.c | normal=0 / preempt_dis PREEMPT=1 / spinlock irqsave→irqs_disabled=128 / tasklet SOFTIRQ=0x100 |
| **D2** completion | comp_demo.c | kthread 睡等、ISR complete 喚醒、計數不合併、複刻 VPE |
| **D3** mutex vs spinlock | mutex_demo.c | in_cs 恆為1(互斥)、等待者睡著0%CPU、持鎖 msleep 合法(spinlock 不行) |
| **D4** 睡眠鎖在中斷爆 | mutex_atomic.c | tasklet 裡 mutex_lock → might_sleep 抓「invalid context」；由 ksoftirqd/0 跑(process 排的無中斷可搭) |
| **D5** 自死鎖 | spin_deadlock.c | 重複拿同鎖 → 單核凍結 → watchdog 重啟(看到 U-Boot 重跑)；真實常是純黑洞無診斷 |

---

## 七、原廠 Novatek 對照（見 memory: nvt-vpe-tasklet-arch）

VPE 中斷架構 = 你 A3+B1+A2 的集大成：
```
nvt_vpe_drv_isr (vpe_drv.c:253) 上半部
  ├ complete(&vpe_completion)   ← 喚醒等硬體的 process(D2 同款)
  ├ tasklet_schedule(&job_tasklet) ← 下半部(A3 同款)
  └ vpe_isr() ← 戳暫存器核心,原始碼不在SDK(binary blob)
job_tasklet/job_lock/linklist_lock 全收進 per-device 結構(kdrv_vpe_int.h:86)
  linklist_lock = B1 的 spin_lock 同款
```
用 tasklet 的驅動：`grep -rl "tasklet_schedule\|tasklet_init" drivers/soc/nvt` → SIE/IFE/IPE/DCE/VPE/JPEG/codec/IVE/MD/DIS

**Novatek 一貫模式**：硬體控制核心(vpe_isr/DMA)鎖在 binary blob，只開放上層膠水碼

---

## 八、板子關鍵配置（.config）

```
CONFIG_WATCHDOG=y              # na51055_wdt 最後防線(D5 救場)
CONFIG_DEBUG_SPINLOCK=y        # spinlock 死前留診斷
CONFIG_DEBUG_MUTEXES=y         # 抓非持有者解鎖
CONFIG_DEBUG_ATOMIC_SLEEP=y    # 抓中斷上下文睡眠(D4 might_sleep)
CONFIG_SOFTLOCKUP_DETECTOR     # 沒開 → 死前多半無警告
CONFIG_PROVE_LOCKING           # 沒開(lockdep) → 抓不到 ABBA
CONFIG_FTRACE                  # 沒開 → 無 /sys/kernel/debug/tracing
```

---

## 九、書(2.6) vs 4.19 過時處總表

| 主題 | 書 | 4.19 |
|---|---|---|
| 中斷 flag | IRQF_DISABLED | 已移除,強制關本地中斷 |
| 中斷入口 | do_IRQ | generic irq framework + GIC |
| 下半部 | BH/task queue | 已移除,剩 softirq/tasklet/workqueue |
| workqueue 執行緒 | events/n(每CPU每類型一條) | CMWQ 共享 kworker pool |
| semaphore API | DECLARE_MUTEX/init_MUTEX | DEFINE_SEMAPHORE/sema_init |
| 互斥睡眠鎖 | 用 count=1 semaphore | 用 mutex |
| 大鎖 | BKL(lock_kernel) | 2.6.39 移除 |
| 架構假設 | x86(強順序) | ARM(弱順序,barrier 真必要) |
