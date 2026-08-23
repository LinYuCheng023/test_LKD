/*
 * container_practice.c  —  自己動手實作 container_of
 *
 * 這是純 user space 程式，跟 kernel 無關（container_of 只是指標算術）。
 * 編譯：  gcc -Wall -o cp container_practice.c
 * 執行：  ./cp
 *
 * 目標：把下面三個 TODO 填完，讓程式能從「成員位址」回推出「外層 struct 位址」。
 * 填對的話，最後驗證會印出  "OK"。
 */

#include <stdio.h>
#include <stddef.h>   /* 標準的 offsetof 就在這（最後一關會用到對照）*/

/* ── 我們自己的精簡版 list_head（模仿 kernel）────────────────── */
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* ── 外層資料 struct：把 list_head 當成員「嵌」進去 ──────────────
 * 故意在 node 前面放了 int + char，讓你體會「對齊/padding 會讓偏移
 * 不等於你心算的值」——這正是為什麼要用 offsetof 而不是自己數。
 */
struct my_entry {
    int               data;
    char              flag;
    struct list_head  node;
    long              extra;
};


/* ============================================================
 * TODO 1：自己實作 my_offsetof
 *   提示：假設 struct 擺在位址 0，那麼「member 的位址」就等於「偏移量」。
 *         只取位址（&），千萬別解參考（不要 *）。
 *   你只能用到：type、member、(type *)0、&、(size_t) 這些零件。
 * ============================================================ */
#define my_offsetof(type, member) (size_t)(&(((type *)0)->member)) /* TODO: 在這裡填一行 */


/* ============================================================
 * TODO 2：自己實作 my_container_of
 *   功能：給「成員的位址 ptr」、「外層型別 type」、「成員名 member」，
 *         回推出外層 struct 的開頭位址。
 *   提示：外層起點 = 成員位址 − 成員偏移量。
 *         記得：指標相減要先轉成 (char *) 才是「以 byte 為單位」。
 *         可以用你上面寫的 my_offsetof。
 *   先用「簡單版」就好（不用寫 ({ }) statement expression / typeof）：
 *         直接寫成一個會算出位址的表達式即可。
 * ============================================================ */
#define my_container_of(ptr, type, member) ({ 					\
		const typeof(((type *)0)->member) *__mptr = (ptr);  	\
		(type *)((char *)(__mptr) - my_offsetof(type, member));})  	/* TODO: 在這裡填 */


int main(void)
{
    struct my_entry e = { .data = 111, .flag = 'X', .extra = 999 };

    /* 先把各成員的位址印出來，親眼看 struct 在記憶體怎麼排 */
    printf("&e        = %p   (struct 開頭)\n", (void *)&e);
    printf("&e.data   = %p\n", (void *)&e.data);
    printf("&e.flag   = %p\n", (void *)&e.flag);
    printf("&e.node   = %p   (我們手上只會有這個)\n", (void *)&e.node);
    printf("&e.extra  = %p\n", (void *)&e.extra);
    printf("\n");

    /* 驗證 TODO 1：你算的偏移 vs 標準 offsetof，應該要一樣 */
    printf("my_offsetof(node) = %zu\n", (size_t)my_offsetof(struct my_entry, node));
    printf("   offsetof(node) = %zu   (標準版，拿來對答案)\n",
           offsetof(struct my_entry, node));
    printf("\n");

    /*
     * 核心戲：假裝我們「只拿到 node 的位址」（就像走訪鏈表時的處境），
     * 用 my_container_of 把整個 my_entry 的開頭算回來。
     */
    struct list_head *only_have_this = &e.node;   /* 假裝這是手上唯一的線索 */

    struct my_entry *recovered =
        my_container_of(only_have_this, struct my_entry, node);

    printf("從 &e.node 回推出的 struct 開頭 = %p\n", (void *)recovered);
    printf("真正的 struct 開頭             = %p\n", (void *)&e);
    printf("\n");
	

    /* ── 自動驗證 ───────────────────────────────────────── */
    int ok = 1;
    if (my_offsetof(struct my_entry, node) != offsetof(struct my_entry, node)) {
        printf("✗ TODO 1 偏移算錯了\n");
        ok = 0;
    }
    if (recovered != &e) {
        printf("✗ TODO 2 回推位址不對\n");
        ok = 0;
    }
    if (ok) {
        /* 進階驗證：透過回推出的指標去改值，看是不是真的同一塊 */
        recovered->data = 1234;
        if (e.data == 1234)
            printf("OK ✓  你的 container_of 正確！（改 recovered->data 真的改到了 e.data）\n");
    }
	
	/* ===== 情況 B：錯誤用法，「嘴上說 node、手卻傳 data 的位址」 =====
     *
     * &e.data 的型別是 int*，但我們卻叫 container_of「從 node 反推」。
     * node 偏移是 8，所以它會把 data 的位址 (= struct 開頭) 再往回減 8，
     * 算出一個「struct 開頭往前 8 byte」的【完全錯誤位址】。
     */
    int *wrong = &e.data;   /* 故意：型別是 int*，不是 list_head* */
 
    /* 簡單版：不會吭聲，默默算出錯位址 */
    /*struct my_entry *bad = my_container_of(wrong, struct my_entry, node);
    printf("[錯誤] unsafe 回推 = %p  ← 比真正開頭少了 8！這是垃圾位址\n", (void *)bad);
    printf("       真正開頭   = %p\n", (void *)&e);

	/*
     * 把下面這行的註解拿掉、重編，看「安全版」遇到同樣的錯誤會怎樣：
     *   它會在編譯期就跳 incompatible pointer type 警告（或 -Werror 直接編不過）
     *   —— bug 根本進不了 runtime。
     */
    /* struct my_entry *blocked = container_of_safe(wrong, struct my_entry, node); */
    /* (void)blocked; */
 
    printf("\n");
    printf("把程式裡 container_of_safe(wrong,...) 那行的註解拿掉再編一次，看差別。\n");
    return 0;
}

/*
 * ── 想再進階？（全部選做）──────────────────────────────────
 * 進階 1：把 my_container_of 改寫成 kernel 那種「安全版」——
 *         用 ({ ... }) statement expression + typeof 做型別檢查。
 *         然後故意傳一個型別不符的 ptr，看編譯器會不會警告。
 *
 * 進階 2：在 struct 裡把 node 移到最前面（偏移 0），重編一次，
 *         觀察 my_container_of 在偏移 0 時是否仍正確（減 0）。
 *
 * 進階 3：用它走訪——自己用這個 struct 串一條三節點的鏈表，
 *         用 node 串起來，再靠 container_of 從 node 取回 data 印出來。
 *         （這就是 CH6 list_for_each_entry 的核心動作！）
 */