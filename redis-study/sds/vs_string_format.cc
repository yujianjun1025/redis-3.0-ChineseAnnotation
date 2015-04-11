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
    char buf[1024];

    format(buf, sizeof(buf), "the buf is create by %s, length is %d\n", "Zeekun xiao", sizeof(buf));

    printf("%s", buf);
    
    return 0;
}
