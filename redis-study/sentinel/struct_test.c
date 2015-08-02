#include <stdio.h>

struct Test
{
    int a;
} test;

typedef struct TypeTest
{
    long b;
} TypeTest;

int main(int argc, char* argv[])
{
    test.a = 1;

    TypeTest.b = 10;// a;

    return 0;
}
