#include <unistd.h>

int main(void)
{
    static const char byte = 'x';

    for (;;) {
        (void)write(STDOUT_FILENO, &byte, sizeof(byte));
        (void)usleep(1000U);
    }
}
