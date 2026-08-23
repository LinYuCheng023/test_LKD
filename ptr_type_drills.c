/*
 * ptr_type_drills.c — 指標 / 型別 / 轉換 強化包
 *
 * 玩法：每個實驗上面有「先預測」的問題，先在心裡（或紙上）寫下答案，
 *       再跑程式對答案。被驚到的地方，就是你以前沒搞清楚的點。
 *
 * 編譯：gcc -Wall -Wextra -o ptd ptr_type_drills.c;cp ptd ~/nfs
 * 編譯：arm-ca9-linux-gnueabihf-gcc -o ptd ptr_type_drills.c;cp ptd ~/nfs

 *      （有幾個實驗會故意觸發 warning，那正是重點，留意編譯輸出！）
 * 執行：./ptd
 *
 * 全部是 user space，跟 kernel 無關，但每個點都標了它在 driver 裡會怎麼咬你。
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

struct my_entry {
    int               data;
    char              flag;
    struct { void *a, *b; } node;   /* 模仿 list_head，兩個指標 */
    long              extra;
};

/* ====================================================================
 * 實驗 1：指標 +1 到底跳幾 byte？
 *
 * 先預測：對 int *pi、char *pc、struct my_entry *ps，
 *         (pi+1) 跟 pi 相差幾 byte？pc+1？ps+1？
 *         （提示：不是都差 1）
 * ==================================================================== */
static void exp1_pointer_stride(void)
{
    int             ai[4];
    char            ac[4];
    struct my_entry ae[4];

    int             *pi = ai;
    char            *pc = ac;
    struct my_entry *ps = ae;

    printf("=== 實驗 1：指標 +1 跳幾 byte ===\n");
    printf("int*   +1 跳 %ld byte\n", (char *)(pi + 1) - (char *)pi);
    printf("char*  +1 跳 %ld byte\n", (char *)(pc + 1) - (char *)pc);
    printf("entry* +1 跳 %ld byte (= sizeof struct = %zu)\n",
           (char *)(ps + 1) - (char *)ps, sizeof(struct my_entry));
    printf(">> 結論：p+1 跳的是 sizeof(*p)，不是 1 byte。\n");
    printf(">> 這就是 container_of 要先轉 (char*) 才減的原因——逼它「以 byte 為單位」。\n\n");
}

/* ====================================================================
 * 實驗 2：指標−指標（得距離） vs 指標−整數（得位址）（你踩過的 Bug 1）
 *
 * 先預測：int arr[5]; 那麼——
 *         (a) &arr[4] - &arr[0]               = ?   （指標 − 指標）
 *         (b) (char*)&arr[4] - (char*)&arr[0]  = ?   （也是指標 − 指標！只是先轉了型別）
 *         (c) &arr[4] - 2                      = ?   （指標 − 整數，注意這個性質不同）
 *         (a)(b) 一樣嗎？(c) 的「結果型別」跟 (a)(b) 一樣嗎？
 * ==================================================================== */
static void exp2_ptr_minus_ptr(void)
{
    int arr[5];

    printf("=== 實驗 2：指標−指標（得距離） vs 指標−整數（得位址）===\n");

    /* (1) 這兩行【都是】指標−指標 → 結果都是整數(相距幾個元素)，差別只在 cast 成什麼型別 */
    printf("[指標−指標] &arr[4] - &arr[0]               = %ld  (單位=int，相距 4 個 int)\n",
           (long)(&arr[4] - &arr[0]));
    printf("[指標−指標] (char*)&arr[4] - (char*)&arr[0] = %ld  (單位=byte，相距 16 byte)\n",
           (long)((char *)&arr[4] - (char *)&arr[0]));
    printf("   ^ 兩行都是指標−指標，結果都是整數(ptrdiff_t)；轉 (char*) 只是把『元素』單位變成 byte。\n");

    /* (2) 這才是指標−整數 → 結果是【位址】(往回移 N 個元素)，型別跟上面完全不同！ */
    printf("[指標−整數] &arr[4]     = %p\n", (void *)&arr[4]);
    printf("[指標−整數] &arr[4] - 2 = %p  (= &arr[2]，往回 2 個 int，結果是【指標/位址】)\n",
           (void *)(&arr[4] - 2));

    printf(">> 結論：指標−指標 → 整數(相距幾個元素)；指標−整數 → 指標(移動後的位址)。兩者性質完全不同。\n");
    printf(">> 你 Bug 1：把該保持「整數」的偏移誤轉成了指標，於是減法從\n");
    printf(">>           『指標−整數(得位址，你要的)』掉進『指標−指標(得距離，錯的)』，語意整個壞掉。\n\n");
}

/* ====================================================================
 * 實驗 3：cast 改了什麼？（cast 不改「數值」，改「怎麼解讀」）
 *
 * 先預測：同一塊 struct，分別用 (char*)、(int*)、原型別 各 −1，
 *         三個算出來的位址會一樣嗎？
 * ==================================================================== */
static void exp3_cast_changes_view(void)
{
    struct my_entry e;
    struct my_entry *p = &e;

    printf("=== 實驗 3：cast 改變的是「步長/解讀」，不是位址值 ===\n");
    printf("p             = %p\n", (void *)p);
    printf("(char*)p - 1  = %p  (退 1 byte)\n",  (void *)((char *)p - 1));
    printf("(int*)p  - 1  = %p  (退 4 byte)\n",  (void *)((int *)p - 1));
    printf("p        - 1  = %p  (退 %zu byte = 一整個 struct)\n",
           (void *)(p - 1), sizeof(struct my_entry));
    printf(">> 結論：cast 當下沒改位址數值，但改了「之後 +/- 跳多遠、* 讀幾 byte」。\n\n");
}

/* ====================================================================
 * 實驗 4：截斷 —— 塞不下會怎樣
 *
 * 先預測：uint8_t x = 300; 印出來是多少？
 *         （uint8_t 只有 8 bit，裝得下 0~255）
 * ==================================================================== */
static void exp4_truncation(void)
{
    uint8_t  x = 300;          /* 故意塞超過 255 */
    uint16_t y = 70000;        /* 超過 65535 */

    printf("=== 實驗 4：型別裝不下 → 截斷 ===\n");
    printf("uint8_t  x = 300   → %u   (300 %% 256)\n", x);
    printf("uint16_t y = 70000 → %u   (70000 %% 65536)\n", y);
    printf(">> 結論：賦值給裝不下的型別，高位被默默砍掉，不會報錯。\n");
    printf(">> driver 裡讀/寫暫存器欄位、湊 32-bit 值時最容易踩，要確認型別寬度夠。\n\n");
}

/* ====================================================================
 * 實驗 5：signed / unsigned 一起比較 —— 經典 kernel bug
 *
 * 先預測：int i = -1; unsigned u = 1;  請問 (i < u) 是真還是假？
 *         （直覺：-1 < 1 當然真……真的嗎？）
 * ==================================================================== */
static void exp5_signed_unsigned(void)
{
    int      i = -1;
    unsigned u = 1;

    printf("=== 實驗 5：signed vs unsigned 比較 ===\n");
    printf("int i = -1; unsigned u = 1;\n");
    printf("(i < u) = %d   ", (i < u));
    printf("%s\n", (i < u) ? "(真)" : "(假！跟直覺相反)");
    printf("原因：比較時 i 被轉成 unsigned，-1 變成 %u（超大），所以 i 反而比 u 大。\n", (unsigned)i);
    printf(">> 這是 kernel/driver 超經典 bug：size 用了 unsigned、又拿 signed 去比，迴圈/邊界整個錯。\n\n");
}

/* ====================================================================
 * 實驗 6：sign extension + 裸 char 的平台差異（你的 ARM 板要特別注意！）
 *
 * 先預測：把一個 bit7=1 的 byte（0x80）讀進 int，會變多少？
 *         signed char 跟 unsigned char 結果一樣嗎？
 *         （而「裸 char」算 signed 還是 unsigned，x86 和 ARM 不一樣！）
 * ==================================================================== */
static void exp6_sign_extension(void)
{
    signed char   sc = 0x80;   /* = -128 */
    unsigned char uc = 0x80;   /* = 128  */
    char          rc = 0x80;   /* 裸 char：x86 多半 signed、ARM 多半 unsigned */

    printf("=== 實驗 6：sign extension 與裸 char 平台差異 ===\n");
    printf("signed char   0x80 → int = %d   (符號擴展成負數)\n", (int)sc);
    printf("unsigned char 0x80 → int = %d   (補 0)\n", (int)uc);
    printf("裸 char       0x80 → int = %d   ", (int)rc);
    printf("%s\n", ((int)rc < 0) ? "(這台 char 是 signed)" : "(這台 char 是 unsigned，像 ARM)");
    printf(">> 結論：讀暫存器/protocol 的 byte，一定用明確的 u8/s8（uint8_t/int8_t），\n");
    printf(">>       別用裸 char 存數值——它的正負號跨平台會變，正是保命守則第⑦條的坑。\n\n");
}

/* ====================================================================
 * 實驗 7（TODO）：void* —— kernel 的通用指標
 *
 * kmalloc 回傳的就是 void*。void* 不帶型別資訊：不能直接解參考、
 * 也不能做指標算術（要先轉成具體型別）。
 *
 * TODO：下面 vp 指向一個 int，把它「正確地」轉回 int* 後印出值。
 *       （把 ??? 換成正確的轉型）
 * ==================================================================== */
static void exp7_voidptr(void)
{
    int   n = 42;
    void *vp = &n;            /* void* 拿著一個 int 的位址 */

    printf("=== 實驗 7：void* 要先轉型才能用 ===\n");

    /* TODO：把 ??? 改成正確寫法，讓它印出 42 */
     int value = *(int *)vp; 
     printf("透過 void* 取回的值 = %d\n", value); 

    printf("(完成 TODO 後取消註解，應印出 42)\n\n");
    (void)vp;  /* 避免未使用警告，填完可刪 */
}

int main(void)
{
    exp1_pointer_stride();
    exp2_ptr_minus_ptr();
    exp3_cast_changes_view();
    exp4_truncation();
    exp5_signed_unsigned();
    exp6_sign_extension();
    exp7_voidptr();
    return 0;
}

/*
 * ── 跑完後的自我檢查 ────────────────────────────────────────
 * 1. 實驗 5 的 (i < u) 你預測對了嗎？沒對的話，這個坑記一輩子。
 * 2. 實驗 6 在你的「PC」跑、跟在「ARM 板」跑，裸 char 那行結果會不會不同？
 *    （在板子上跑跑看，親眼驗證 ARM 的 char 是 unsigned）
 * 3. 想再進階：把實驗 5 的 unsigned 換成 size_t，
 *    寫一個「for (int i = n-1; i >= 0; i--)」但 n 是 size_t 的迴圈，
 *    看會不會變成無窮迴圈（這是真實世界天天有人踩的 bug）。
 */