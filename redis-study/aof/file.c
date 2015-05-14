#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(int argc, char * argv[])
{
/*
    char write_buf[] = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$11\r\nxiaozhihong\r\n";
    FILE *fp = fopen("test.aof", "a+");
    if (fp == NULL)
    {
        printf("file open failed\n");
        return -1;
    }

    ssize_t written = fwrite(write_buf, strlen(write_buf), 1, fp);
    if (written < 0)
    {
        printf("write file error\n");
        return -1;
    }
*/

    FILE *fp_read = fopen("test.aof", "r");
    if (fp_read == NULL)
    {
        printf("file open to read failed\n");
        return -1;
    }

    char buf[128];
    char *line;
    while ((line = fgets(buf, sizeof(buf), fp_read)) != NULL)
    {
        printf("%d:%s", ftello(fp_read), buf);
    }

    char read_buf[128];
    ssize_t readbytes = fread(read_buf, sizeof(read_buf), 1, fp_read);
    printf("reaadbytes = %d\n", readbytes);


    truncate("test.aof", 10);

    return 0;
}
