#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

struct itemhash
{
    u_int64_t hash;
    char fullname[256];
    int base_fd;
    int cfd;
};

typedef struct itemhash i_hash;
