# Kernel 筆記 CH11–12 + 雙核實驗（NT98538A / arm64）

> 平台：**NT98538A / NS02302**，雙核 **Cortex-A53 ARMv8 64-bit**，Linux **6.12.57**
> 對照舊板：NT98525 單核 Cortex-A9 ARMv7 32-bit / 4.19（見 kernel_notes_ch7-10.md）
> 方法：讀書(LKD) → 對照 tree 實碼 → 板子實測（單核+雙核對照）

---

## 一、CH11 定時器與時間管理

### tickless（書沒講，最大更新）
- 舊板 4.19 已是 `NO_HZ_IDLE`（idle 關 tick 省電）；HZ=100
- HZ 不再保證「每 10ms 中斷」——忙時才 tick，idle 時可能好幾秒才醒一次
- `/proc/uptime` 第二個數=idle 時間；tickless 下 jiffies < uptime×HZ

### jiffies（書沒過時，照學）
- `jiffies` = 開機到現在節拍數，秒數 = jiffies/HZ
- 32位 HZ=100 → **497 天迴繞**（IP camera 長跑真會遇到）
- `jiffies`(32) 是 `jiffies_64`(64) 低32位（linker 魔法）
- **★ 比較時間點絕不用 `>`/`<`，用 `time_after/time_before`**（轉 signed 相減，迴繞安全）

### 硬體時鐘（你 ARM 非書的 x86）
- RTC = SoC 內建（舊板 `nvt_rtc f0060000.rtc`），電池續命、開機初始化牆上時間
- clocksource = `arm_global_timer`（ARM 架構計時器，奈秒精度，`ktime_get` 靠它）
- 書的 x86 PIT/APIC/TSC 跟你無關

### 動態定時器（★ API 大改 4.19→）
```
4.19: init_timer + .data + fn(unsigned long)
5.x+: timer_setup(&t,fn,flags) + from_timer(d,t,field) + fn(struct timer_list*)
```
- timer callback 在 **TIMER_SOFTIRQ**（softirq 上下文，不能睡）
- 清理用 `del_timer_sync`；改超時用 `mod_timer`（別手動 del+add）

### 延遲決策樹
- 極短(<1ms) → `udelay/mdelay`（忙等，燒 CPU）
- 長且能睡 → `msleep`（= schedule_timeout 底層，睡等）
- 長不能睡 → 忙等 + `cond_resched()`

### 兩種時間
- 牆上時間 `ktime_get_real_ts64`（會跳，給 user/timestamp）
- 單調時間 `ktime_get`（只增，**算間隔用這個**）；書的 timeval→timespec64（Y2038）

### 板子 timer 精度發現（D6/hrtimer 實測）
- **板子 timer resolution = 10ms**（`Clock Event Device: <NULL>`，沒開 HIGH_RES_TIMERS）
- hrtimer_jitter 實測：timer_list 要 10ms 跑成 20ms(N tick 變 N+1)、抖動 ±10ms；hrtimer 抖動 422us（好但仍受 10ms 限）
- hrtimer 要 5ms → 做不到（被 10ms 粒度拉成 10ms）
- **結論**：影像精密時序靠專用硬體(sensor/VPE 自己的時鐘)，不靠 CPU timer；CPU timer 10ms 夠粗用

---

## 二、CH12 記憶體管理（接回最早的 DMA/cache 線）

### 三種分配的本質
| | 虛擬連續 | 物理連續 | 能給DMA | 用途 |
|---|---|---|---|---|
| kmalloc | ✅ | ✅ | ✅ | 一般（直接映射區/lowmem）|
| vmalloc | ✅ | ❌散 | ❌ | 大塊純軟體(module載入)|
| dma_alloc_coherent | ✅ | ✅+uncached | ✅✅ | DMA 一致性 |

- **DMA 要物理連續**：DMA 不經 MMU/頁表、只看物理位址 → vmalloc(物理散)不能給 DMA
- kmalloc 在**直接映射區**(虛擬=物理+固定偏移)→ 物理連續、`virt_to_phys` 減偏移即可
- vmalloc 物理散 → 要 `vmalloc_to_page` 查頁表（ian_clk 驗證過）
- **MMU 開著→CPU 只能用虛擬地址**；直接映射區只是「最簡單的線性虛擬地址」

### cache 一致性（stale demo 的核心）
- CPU 走 cache、DMA 走 DRAM → 不同步 → stale
- 解法：coherent(uncached) 或 streaming(dma_map_single 手動 flush/invalidate)
- **vmap 雙映射(同物理頁 cached+uncached)在 ARM = unpredictable → crash**（舊板踩過）

### zone / buddy
- 舊板 meminfo：**HighMem=0**（內存小全是 lowmem，沒 kmap 麻煩）
- `/proc/buddyinfo` = 每個 order 有幾塊連續頁；大塊 kmalloc 看高 order 夠不夠
- ZONE_DMA = `GFP_DMA` 拿的，給受限 DMA 設備

### GFP flag（A3/D4 的坑）
- **`GFP_KERNEL`**：會睡，只能 process context
- **`GFP_ATOMIC`**：不睡，中斷/softirq/tasklet/持鎖用（成功率較低）
- 中斷上下文 kmalloc(GFP_KERNEL) → 崩（會睡）

### slab → 你板子是 SLUB
- kmalloc 建在 slab 之上（kmalloc-64/128... 通用 cache）
- **你板子 `CONFIG_SLUB=y`**（非書的 SLAB）→ `/proc/slabinfo` 不存在（SLUB_DEBUG 沒開）
- 看用量：`cat /proc/meminfo | grep Slab`
- coloring 防 false sharing（呼應 cache bouncing）

### 內核棧
- 小而固定（arm 約 8KB）；`KernelStack` meminfo 看得到
- **溢出無聲破壞**（覆蓋 thread_info+相鄰資料）→ 大結構用 kmalloc 別放棧（sensor_dev 用 kzalloc）
- kmalloc「堆」= 直接映射區（不是 user 的 malloc heap）；user 棧能大是虛擬+lazy+page fault 增長

### per-CPU（單核意義小）
- 每核一份→免鎖（唯一要求關搶佔 `get_cpu/put_cpu`）+ 減 cache 抖動
- **★ 編譯時 `DEFINE_PER_CPU` 不能在 module 用**（特殊段）→ module 要用 `alloc_percpu`(運行時)
- 單核 atomic 就夠（sensor 做對）

### ★ 影像 buffer 大塊連續內存 = nvtmpp 私有預留（獨家發現）
- 舊板 cmdline `512M` 但 MemTotal 只 160MB → **352MB 被切走給影像**
- 手法：device tree **私有 node `/nvt_memory_cfg/bridge`**（Linux MM 不認得，非標準 reserved-memory）→ 所以 dmesg 沒印 reserved/CMA
- `nvtmpp_platform.c`：`of_find_node_by_path` 讀 reg=<物理位址,大小> → `ioremap_cache` 映射進來 → 自己當分配器切物理連續大塊給 VPE/ISP/codec
- 另有 8MB 標準 CMA(CmaTotal)並存
- **RAM vs flash**：CH12 全講 RAM；flash/SD 走 VFS→檔案系統→塊設備(CH13/14)；交匯點=page cache(meminfo 的 Cached)

---

## 三、★ 4.19/32bit → 6.12/arm64 移植的 API 差異（實戰）

| API | 舊(4.19) | 新(6.12) | 影響檔案 |
|---|---|---|---|
| `DECLARE_TASKLET` | 3參 `(name,fn,data)` | **2參 `(name,fn)`** | preempt_scope/gpio_irq_tasklet/mutex_atomic |
| tasklet callback | `fn(unsigned long)` | **`fn(struct tasklet_struct*)`** | 同上 |
| `class_create` | `(THIS_MODULE,name)` | **`(name)`** | ian_clk/sensor_sample(_todo) |
| platform `.remove` | 回 `int` | **回 `void`** | ian_clk |
| `vmalloc/vfree` | 間接引入 | **要明確 `#include <linux/vmalloc.h>`** | ian_clk |
| 編譯檢查 | 寬鬆 | **`-Werror`**（void return 0/unused 變數都擋）| mutex_demo 等 |

- Makefile：`CROSS_COMPILE=aarch64-ca53-linux-gnu-`、`ARCH=arm64`、KERNEL_DIR 指新樹
- vermagic 要對（module 跟 kernel 同源才能 insmod）

---

## 四、★ 雙核實驗（單核做不到的）

### D1 preempt_count（arm64 差異）
- 三欄佈局跟 arm32 一樣（PREEMPT/SOFTIRQ/HARDIRQ）
- **`irqs_disabled`：arm64=1（normalized）vs arm32=128（CPSR I bit=0x80）**
- spin_lock 在雙核「真的自旋等另一核」（單核是假的、優化掉）

### D3 mutex vs spinlock（真並行）
- top 抓到兩 worker 分別在 **CPU0/CPU1**（`ps` 的 CPU 欄）→ 真並行鐵證
- in_cs 恆為 1 → mutex 在雙核「真的防兩核同時進臨界區」（不像單核只防搶佔）

### D5 死鎖（雙核死法不同）
- **自死鎖**：一核卡死、另一核還活 → **半癱瘓殭屍態掙扎 16 秒**（影像/音訊全 -15、timer losing event）→ 才重啟。單核是瞬間全黑
- **★ ABBA（雙核專屬）**：A@CPU0 持 lockA 等 lockB、B@CPU1 持 lockB 等 lockA → 兩核全死 → 秒重啟。單核做不出 ABBA（不可能兩 thread 同時各持一鎖）
- 洞察：多核死鎖「更難察覺」——系統看似還活(在印 log)其實半廢

### 內存屏障（3.1，單核完全做不到）
- 綁核(kthread_bind) + READ_ONCE + smp_wmb/rmb + 握手 + 遞增序號
- **use_barrier=1 → 21億次 mismatch=0；use_barrier=0 → 也 21億次 0**
- **★ A53 抓不到乱序**：in-order 執行 + store buffer 小/FIFO 順序刷出 → 幾乎不 store-store 亂序
- 但架構「允許」弱序 → 換大核(A72/A76)同 code 會爆 → **仍須加屏障(可移植)**
- 教訓：正確寫法 > 抓到乱序表演；A53 溫和≠可不加屏障

### false sharing（單核做不到）
- 兩核狂加各自計數器，同 cache line vs `____cacheline_aligned` 分開
- **share=1 ≈ share=0（<2% 差異）** → A53 溫和（少核、距離近、coherency 便宜）
- 大核會慢 3~10 倍（伺服器效能殺手），A53 可忽略；cache line=64 bytes

### ★ B1 撕裂（雙核真並行，這個成功了！）
- 兩核 writer/reader 狂寫讀多欄位 {a,b,c}
- **use_lock=0 → 2.2億次讀 tear=1757萬（8% 撕裂！）；use_lock=1 → 2.48億次 tear=0**
- **★ 為什麼這個 A53 抓得到（屏障/false sharing 抓不到）**：撕裂是**正確性問題**——只要「兩核真並行+多欄位分開寫」，「寫一半」的時間窗口**邏輯上必然存在**，跟 A53 溫不溫和無關；乱序/false sharing 是微架構效能現象，A53 溫和就輕微

### ★★ 「8% 撕裂」賭局（tear_crash，親眼看災難）
- 撕裂讀「小buf 指針 + 大 len」→ `memset` 越界寫
- **第一把(BIG_SZ=4096)**：沒當場炸,但默默踩爛影像串流 `send_buffer_video`→AVSYNC 線程退出（**兇手現場分離、延遲爆炸**）
- **第二把(BIG_SZ=65536)**：`Unable to handle kernel write to read-only memory` → **arm64 Oops**！(踩到唯讀頁,MMU 擋下)
- **越界寫兩種命運**：踩可寫記憶體→默默損壞(難查)；踩唯讀頁→當場 Oops(好查)
- Oops 只殺肇事 kthread(kill offender),系統帶傷續跑(Tainted),非 panic
- **結論**：8% 不是「92%能用」,是「隨時引爆,死法隨機抽(默默損壞/Oops/panic)」；多欄位一致=必須鎖,不能賭

### 讀 arm64 Oops（debug 技能）
- `pc`=爆在哪函式、`lr`=誰呼叫、Call trace=呼叫鏈
- ARM 慣例：**x0~x7=函式參數**(x0第1參…)、x19~x28=callee-saved(函式內變數)、x30=lr、x29=fp
- memset(x0=目標,x1=值,x2=長度)；但 x2/x8 在 memset 內會邊寫邊變(x8=當前寫入位址=爆點)
- `WnR=1`=寫造成、`FSC=permission fault`=權限錯、`Comm`=哪個 thread、`CPU:`=哪核

---

## 五、★ 貫穿的核心洞察：A53 溫和 vs 正確性

| 實驗 | 類型 | 大核會怎樣 | A53 結果 | 為什麼 |
|---|---|---|---|---|
| 內存屏障 | 微架構(乱序) | 容易抓 | 21億次0 | A53 in-order 不亂序 |
| false sharing | 微架構(cache) | 慢3~10倍 | <2% | A53 coherency 便宜 |
| **B1 撕裂** | **正確性(竞态)** | 撕裂 | **8% 撕裂** | **邏輯窗口必然存在,與硬體無關** |

**最深的一課**：
- **正確性 bug（竞态/撕裂）**：跟硬體溫不溫和無關,有並發窗口就必然發生 → **一定要防,不能賭**
- **效能現象（乱序/false sharing）**：看硬體,A53 溫和所以輕微,大核才嚴重 → 但 code 都要寫對(可移植)
- A53(in-order/少核/coherency簡單)讓「教科書的多核陷阱」都很淺 → 但換平台就爆

---

## 六、板子關鍵 config（NT98538A / 6.12）

```
CONFIG_SLUB=y                  # 非 SLAB → 無 /proc/slabinfo
CONFIG_LOCKDEP_SUPPORT=y       # 只是支援
CONFIG_PROVE_LOCKING not set   # ★ lockdep 沒開 → 3.3 做不了(重編才行)
CONFIG_DEBUG_SPINLOCK not set
CONFIG_DEBUG_MUTEXES=y         # D4 mutex 違規檢測
CONFIG_DEBUG_ATOMIC_SLEEP=y    # scheduling while atomic 檢測
/proc/config.gz 不存在         # config 沒編進 kernel
```
硬體：雙核 A53、cache line 64 bytes、timer resolution 10ms(無 HIGH_RES_TIMERS)、sensor imx464
