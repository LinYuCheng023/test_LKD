#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define SIZE 20 * 1024 * 1024
int main(void) {
    char *p = malloc(SIZE);   // 要 64MB,但先不碰
    printf("malloc done, pid=%d\n", getpid());
    //getchar();                             // 停住,等你觀察
    for (int i = 0; i < SIZE; i += 4096)
        p[i] = 1;                          // 每頁踩一下
    printf("touched\n");
    //getchar(); 	// 再停,再觀察
	free(p);
    return 0;
}