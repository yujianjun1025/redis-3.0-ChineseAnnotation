#include <stdlib.h>
#include <stdio.h>

int main(int argc, char* argv[])
{
    char* p = (char*)malloc(1000000);

    /// 默认的malloc也会记录分配的内存大小
    /// 系统给我们的不一定是刚好10000000,只是从堆上拿出最合适大小的一块
    printf("real malloc size = %d\n", *(size_t*)((char*)p - sizeof(size_t)));
    printf("%d\n", malloc_size(p));

    free(p);

    return 0;
}
