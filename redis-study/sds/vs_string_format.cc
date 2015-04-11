//author: Zeekun Xiao

#include <stdio.h>
#include <stdarg.h>

void format(char* buf, size_t buflen, char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    vsnprintf(buf, buflen, fmt, ap);

    va_end(ap);
}

int main(int argc, char* argv[])
{
    char buf1[1024];
    char buf2[1024];

    format(buf1, sizeof(buf1), "the buf is create by %s, length is %d\n", "Zeekun xiao", sizeof(buf1));
    printf("%s", buf1);
    
    format(buf2, sizeof(buf2), "only string\n");
    printf("%s", buf2);

    return 0;
}
