#include <stdio.h>

int main(void) {
    unsigned int *p = (unsigned int *)main;
	printf("main at %p\n",(void *)p);
	*p = 0;
	
    return 0;
}