#include <stdio.h>
#include <sys/resource.h>

int main() {
    struct rlimit lim;

    if (getrlimit(RLIMIT_STACK, &lim) == 0)
        printf("stack size: %d\n",  (int) lim.rlim_cur);

    if (getrlimit(RLIMIT_NPROC, &lim) == 0)
        printf("process limit: %d\n", (int) lim.rlim_cur);

    if (getrlimit(RLIMIT_NOFILE, &lim) == 0)
        printf("max file descriptors: %d\n", (int) lim.rlim_cur);

    return 0;
}
