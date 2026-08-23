# Linux Kernel Development — 第 6 章 內核資料結構（embedded 伴讀筆記）

> 對應書：Robert Love《Linux Kernel Development, 3rd》第 6 章
> 視角：embedded driver（Novatek NT98525 32-bit / NT98538 64-bit）
> 內容：四大資料結構 + 選擇指南 + Big-O + 四隻真板實作模組 + 踩過的坑

**核心精神**：kernel 已內建通用資料結構，**重用它們、別造輪子**（§6.1 開頭 & §6.7 都強調）。

---

## 目錄
1. [共同心法：侵入式資料結構](#零共同心法侵入式資料結構)
2. [§6.1 鏈表 list](#一61-鏈表-list)
3. [§6.2 佇列 kfifo](#二62-佇列-kfifo)
4. [§6.3 映射 idr（★新舊 API 大不同★）](#三63-映射-idr新舊-api-大不同)
5. [§6.4 二叉樹 rbtree](#四64-二叉樹-rbtree紅黑樹)
6. [§6.5 選擇指南](#五65-選擇指南)
7. [§6.6 Big-O](#六66-演算法複雜度-big-o)
8. [四隻模組總覽 + 通用坑](#七四隻模組總覽--通用坑)

---

## 零、共同心法：侵入式資料結構

list 和 rbtree 都用**侵入式（intrusive）**設計——把「鏈結節點」嵌進**你自己的**資料 struct：

```c
struct my_data {
    int  value;
    struct list_head node;   // list 的鏈結能力嵌進來
    // 或 struct rb_node node;  // rbtree 同理
};
```

**取回外層靠 `container_of`**（rbtree 叫 `rb_entry`，就是 container_of）：

```c
#define container_of(ptr, type, member) ({                  \
    const typeof(((type*)0)->member) *__mptr = (ptr);       \  // 型別防呆
    (type*)((char*)__mptr - offsetof(type, member)); })        // 成員位址 − 偏移
```

- `offsetof`：假設 struct 在位址 0 → 成員位址就等於偏移；只取位址不解參考，故安全。
- padding/對齊會影響偏移 → **一律用 offsetof、不手算**。
- 好處：**一套 API 通吃所有型別**（C 沒泛型的變通）。

---

## 一、§6.1 鏈表 list

kernel 標準鏈表 = **環形雙向**（走訪終止是「繞回 head」，不是 `== NULL`）。

### 初始化三兄弟
| 巨集 | 做什麼 | 用在 |
|---|---|---|
| `LIST_HEAD(name)` | 定義+初始化一個 head | 建整條鏈表入口 |
| `INIT_LIST_HEAD(ptr)` | 執行期初始化既有 list_head | 動態節點（kmalloc 後）|
| `LIST_HEAD_INIT(name)` | struct 初始化器用的初值 | 靜態定義 struct |

「頭節點」本身也只是普通 `list_head`，只當入口。

### 操作 API（全 O(1)，只改鄰居指標）
| 動作 | API | 注意 |
|---|---|---|
| 加到頭後（stack/LIFO）| `list_add(new, head)` | |
| 加到尾（queue/FIFO）| `list_add_tail(new, head)` | driver 常用 |
| 刪除 | `list_del(entry)` | **只移除，不 kfree！自己 kfree** |
| 刪除+重置 | `list_del_init(entry)` | 節點還要再用時 |
| 搬到別條 | `list_move(entry, head)` | = del+add |
| 判空 | `list_empty(head)` | 空→非0 |

### 走訪
| 巨集 | 用途 | 給你 |
|---|---|---|
| `list_for_each_entry(pos, head, member)` | **常用**正向 | 外層 struct 指標 |
| `list_for_each_entry_reverse(...)` | 反向 | |
| `list_for_each_entry_safe(pos, n, head, member)` | **邊走邊刪** | 多帶 n 暫存下一個 |

`member` = list_head 在 struct 裡的**成員名**（不是型別）。

**展開後**：`list_for_each_entry` =「container_of 從 head->next 反推 → 沿 next 走 → 繞回 head 停」；`_safe` 多一個 `tmp`，**進迴圈 body 前先把下一個抄進 tmp**，故當前節點被 kfree 也安全。

🔴 `_safe` 只防「自己迴圈內刪」，**並發存取仍要上鎖**（第⑥條）。

### 標準 cleanup
```c
list_for_each_entry_safe(entry, tmp, &my_list, node) {
    list_del(&entry->node);
    kfree(entry);            // 普通版這裡 use-after-free
}
```

---

## 二、§6.2 佇列 kfifo

生產者/消費者 FIFO。靠 **in/out 兩偏移**：`in==out` 空、`in-out==size` 滿。

**為什麼有固定容量**：`kfifo_alloc` 時就配好一塊**固定大小連續環形 buffer**，塞滿就滿。好處：操作**不配記憶體**（快、**可在 ISR 用**、環形重用）。對比 list 每節點現場 kmalloc、無固定上限（受記憶體限制）。

### API
| 動作 | API | 注意 |
|---|---|---|
| 動態建立 | `kfifo_alloc(&f, size, gfp)` | **size 必須 2 的次方** |
| 自備 buffer | `kfifo_init(&f, buf, size)` | buffer 自己 free |
| 推入 | `kfifo_in(&f, from, len)` | **檢查回傳值**（滿了少塞/回 0）|
| 取出 | `kfifo_out(&f, to, len)` | **檢查回傳值**（空了少拿）|
| 偷看 | `kfifo_out_peek(&f, to, len, 0)` | 不移動 out 偏移 |
| 剩空間/資料量 | `kfifo_avail` / `kfifo_len` | avail=空位 |
| 空/滿 | `kfifo_is_empty` / `kfifo_is_full` | |
| 清空/釋放 | `kfifo_reset` / `kfifo_free` | free 只配對 alloc |

### 三個坑
1. `size` 必須 **2 的次方**（內部用位元遮罩環繞）。
2. `kfifo_in`/`out` **回傳值一定要檢查**。
3. `alloc`↔`free`；`init` 自備 buffer 自己 free。

**真板驗證**：128÷4 = 32 個；第 33 個 → `kfifo_in` 回 0 → `-ENOSPC` → shell "No space left on device"。

---

## 三、§6.3 映射 idr（★新舊 API 大不同★）

idr =「整數 ID → 指標」映射，招牌是**自動生成唯一 ID**。

### 🔴 書的 API 已作廢，用 5.4 新 API
| 書（2.6 舊）| 5.4 新 API |
|---|---|
| `idr_pre_get()` | **已刪除** |
| `idr_get_new()` | `idr_alloc()` |
| `idr_get_new_above()` | `idr_alloc_cyclic()` |
| `idr_remove_all()` | **已刪除**（用 for_each / destroy）|
| `idr_find/remove/destroy/init` | 同名沒變 ✅ |

### 新 API
```c
DEFINE_IDR(my_idr);
int id = idr_alloc(&my_idr, ptr, 0, 0, GFP_KERNEL);  // start,end(0=無上限)
//   成功=id(≥0)、失敗=負碼；★一行取代舊的 pre_get+do-while★
void *p = idr_find(&my_idr, id);      // 找不到回 NULL
idr_remove(&my_idr, id);              // ★不 kfree 物件★
idr_for_each_entry(&my_idr, e, id) { kfree(e); }
idr_destroy(&my_idr);
```

### 重點
- **鎖用 mutex 不是 spinlock**：`idr_alloc(GFP_KERNEL)` 可能睡（第⑤條）。
- 別把 NULL 綁進去（find 找不到也回 NULL）。
- ID 上限 `INT_MAX`（ID 用 int、負值留錯誤碼）；`end` 開區間。
- `idr_alloc` 補洞（重用、緊湊）；`idr_alloc_cyclic` 不補洞（避免號碼太快重用）。
- 4.20+ 有 **xarray** 逐步取代；5.4 用 idr 即可。

---

## 四、§6.4 二叉樹 rbtree（紅黑樹）

**保證 O(log n) 的「有序」容器**。適合「大量資料 + 頻繁快速查找 + 需要有序」。

### 自平衡直覺（六條規則不用背）
- **變色 + 旋轉**維持半平衡：新節點先塗紅；紅連紅時修——叔叔紅→變色（往上冒泡）、叔叔黑→旋轉（保序重整、壓低高度）。
- 著色規則保證「最長 ≤ 2×最短」→ 高度 O(log n)。
- **這整套由 `rb_insert_color`/`rb_erase` 幫你扛**。

### 🔴 特色：search/insert 要自己寫（C 沒泛型）
```c
struct rb_root root = RB_ROOT;
struct my_node { struct rb_node node; unsigned long key; ...; };

// 搜尋
struct rb_node *n = root.rb_node;
while (n) {
    struct my_node *it = rb_entry(n, struct my_node, node);
    if (key < it->key)      n = n->rb_left;
    else if (key > it->key) n = n->rb_right;
    else                    return it;
}

// 插入
struct rb_node **p = &root.rb_node, *parent = NULL;
while (*p) {
    struct my_node *it = rb_entry(*p, struct my_node, node);
    parent = *p;
    if (new->key < it->key)      p = &(*p)->rb_left;
    else if (new->key > it->key) p = &(*p)->rb_right;
    else                         return -EEXIST;
}
rb_link_node(&new->node, parent, p);   // ① 掛上
rb_insert_color(&new->node, &root);    // ② 再平衡（黑箱）
```

### 其他 API
- `rb_erase(&node->node, root)`：移除（**只移除不 kfree**）。
- `rb_first(root)` + `rb_next(node)`：**有序**走訪（招牌）。
- `rbtree_postorder_for_each_entry_safe(...)`：清空用（先子後父，安全 kfree）。

**真板驗證**：亂序插入 `5,2,8,0` → dump 出來 `0,2,5,8`（依 key 排序）。

---

## 五、§6.5 選擇指南

| 需求 | 選 |
|---|---|
| 走訪為主、量少、不趕時間 | **list** |
| 生產者/消費者、FIFO、可定長 | **kfifo** |
| 整數 UID → 物件、要發號 | **idr** |
| 大量資料 + 頻繁快速查找 + 有序 | **rbtree** |

**關鍵取捨**：
- 大小未知/可能很多 → list；可接受定長 → kfifo。
- **查找不頻繁 / 量少 → 用 list 就好**（別為理論快扛 rbtree 複雜度）。
- 原則：**用「最簡單能完成工作」的那個**。
- 都不合適 → radix tree / bitmap / 自寫 hash（自造輪子最後手段）。

---

## 六、§6.6 演算法複雜度 Big-O

描述「輸入變大時時間怎麼漲」（伸縮性），非絕對速度。

| 複雜度 | 名稱 | 本章實例 |
|---|---|---|
| O(1) | 恆定 | list_add/del、kfifo_in/out |
| O(log n) | 對數 | **rbtree 查找**（21 億筆約 31 步）|
| O(n) | 線性 | 走訪鏈表 / rbtree 有序走訪 |
| O(n²)+ | 平方/指數/階乘 | 避開 |

🔴 **實務智慧**：Big-O 忽略常數 c——
- O(1) ≠ 快（可能恆定地慢）。
- **輸入小時 O(n) 可能比 O(1) 快**。
- 判斷要**複雜度 + 典型輸入大小一起看**，別為用不到的伸縮度盲目優化。

---

## 七、四隻模組總覽 + 通用坑

### 真板實作（NT98525）
| 資料結構 | 模組 | 招牌驗證 |
|---|---|---|
| list | `ian_list_demo.c` | FIFO 正序 |
| kfifo | `kfifo_demo.c` | 先進先出 + 空/滿邊界（第 33 個 dropped）|
| idr | `idr_demo.c` | 自動發號 0/1/2 |
| rbtree | `rb_demo.c` | 亂序插入 → 有序輸出 |

### 貫穿四個結構的通用坑（回扣保命守則）
1. **「移除」≠「釋放」**：`list_del` / `idr_remove` / `rb_erase` 都**只移除、不 kfree** → 自己 `kfree`（第③條）。
2. **容器滿了用回傳值告知**：`kfifo_in` 回 0、`idr_alloc` 回 `-ENOSPC` → **一定要檢查**，失敗路徑 kfree 掉沒用到的資源。
3. **鎖看「會不會睡」**：不睡 → spinlock（list/kfifo/rbtree）；鎖內配可睡記憶體（`idr_alloc(GFP_KERNEL)`）→ mutex（第⑤⑥條）。
4. **`_safe` ≠ thread-safe**：邊走邊刪用 `_safe`；並發另外上鎖。
5. **errno → user 訊息**：回 `-ENOSPC`/`-EINVAL`/`-ENOENT`，user 看到對應字串。
6. **書老要對照原始碼**：idr 整套在 5.4 已改寫，照書打會編不過（第⑦條）。

---

## 收尾
CH6 四大資料結構全部真板實作 + 驗證完成。侵入式設計（container_of/rb_entry）、複雜度取捨、重用而非造輪子——這章心法會一路用到排程、記憶體、網路各子系統。
