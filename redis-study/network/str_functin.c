#include <string.h>
#include <stdio.h>

int main(int argc, char* argv[])
{
    int ret = 0;
    char *p = "If there is a will, there is a way!\r\n";

    char *pos = strchr(p, '\r');

    if (pos != NULL)
    {
        printf("pos - 2 = %c\n", *(pos - 2));
        printf("pos - 1 = %c\n", *(pos - 1));
        printf("pos = %c\n", *pos);
        printf("pos + 1 = %c\n", *(pos + 1));
    }
    else
    {
        printf("pos = NULL\n");
    }

    return ret;
}
