#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <poll.h>
#include <sys/sendfile.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include "sendget.h"

typedef struct storagehandle
{
    char fsname[1024];
    char cachename[1024];
    char objname[256];
    int cfd; // cachefd
    void *cachemem;
    int fsfd; // backing file system fd
    u_int64_t hits;
    size_t size;
} st_t;

typedef struct hashentry
{
    u_int64_t fullhash;
    off_t index;
} he_t;

typedef struct itemhash
{
    u_int64_t hsize;
    u_int64_t key;
    he_t *indexes;
} ih_t;

typedef struct storage
{
    char backingdir[80];
    char cachedir[80];
    size_t cached;
    size_t cachelimit;
    st_t *items;
    ih_t hash;
} s_t;

#define BACKINGDIR "./back"
#define CACHEDIR "./cached"
#define OP_FDPASS '1'
#define OP_COPY '2'
#define OP_SPLICE '3'
#define MAX_THREADS 16

#ifdef DEBUG
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

sem_t sem;

u_int64_t strtohash(char *str)
{
    u_int64_t hash = 0;
    int rots = 0;
    while (*str)
    {
        hash = (hash << rots) | (hash >> (64 - rots)) | *str;
        rots = (rots + 1) % 64;
        str++;
    }
    return hash;
}

void put_item(s_t *s, off_t index, ssize_t size)
{
    u_int64_t fullhash = strtohash(s->items[index].fsname);
    off_t tindex = fullhash % s->hash.hsize;

    while (s->hash.indexes[tindex].fullhash)
        tindex = (tindex + 1) % s->hash.hsize;

    // we have a clear slot
    s->hash.indexes[tindex].fullhash = fullhash;
    s->hash.indexes[tindex].index = index;
    s->items[index].size = size;
    char fullname[1024];
    sprintf(fullname, "%s/%s", BACKINGDIR, s->items[index].fsname);
    s->items[index].fsfd = open(fullname, O_RDONLY);
    if (s->items[index].fsfd <= 0)
    {
        printf("couldn't open %s\n", fullname);
        perror("open");
        exit - 1;
    }
    char cachename[1024];
    sprintf(cachename, "%s/%s", CACHEDIR, s->items[index].fsname);
    s->items[index].cfd = open(cachename, O_RDONLY);
    dprintf("%s %d\n", cachename, s->items[index].cfd);
    if (s->items[index].cfd < 0)
    {
        s->items[index].cfd = 0;
        // s->items[index].cachemem = mmap(NULL, size, PROT_READ, MAP_SHARED, s->items[index].fsfd, 0);
    }
    else
    {
        close(s->items[index].fsfd);
        // s->items[index].cachemem = mmap(NULL, size, PROT_READ, MAP_SHARED, s->items[index].cfd, 0);
        s->items[index].fsfd = 0;
    }

    if (0 && index == 5)
    {
        u_int64_t sum = 0;
        int tval;
        while (read(s->items[index].fsfd, &tval, sizeof(int)) > 0)
        {
            sum += tval;
            printf("%ld\t", tval);
        }

        printf("idx %ld name %s sum %ld\n", index, fullname, sum);
    }

    dprintf("init %s, size %ld\n", fullname, s->items[index].size);
}

void uncache_item(s_t *s, off_t index)
{
    unlink(s->items[index].cachename);
    s->items[index].cfd = 0;
}

void cache_item(s_t *s, off_t index)
{
    struct stat st;
    char fullpath[1024];
    sprintf(fullpath, "%s/%s", BACKINGDIR, s->items[index].fsname);
    sprintf(s->items[index].cachename, "%s/%s", CACHEDIR, s->items[index].fsname);
    printf("cache %s to %s\n", fullpath, s->items[index].cachename);
    if (stat(fullpath, &st) == 0)
    {
        if (S_ISREG(st.st_mode)) // If is a regular file
        {
            // open
            int cfd = open(s->items[index].cachename, O_RDWR | O_CREAT, S_IRGRP | S_IRUSR | S_IWUSR);
            perror("ocache");
            // copy
            ssize_t copysize = st.st_size;
            ssize_t rb;
            // char buf[1024 * 1024];
            if (s->cachelimit < s->cached + copysize)
            {
                // while (rb = read(s->items[index].fsfd, buf, sizeof(buf) > 0))
                //     write(cfd, buf, rb);
                perror("cfr ");
                s->items[index].cfd = cfd;
                s->items[index].size = copysize;
                s->cached += copysize;
            }
        }
    }

    s->items[index].cfd = open(s->items[index].cachename, O_RDONLY);
}

void init_storage(s_t *s)
{
    // walk backingdir and add objects
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    if ((dir = opendir(BACKINGDIR)) == NULL)
    {
        perror("opendir");
    }
    off_t nfiles = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        // printf("%s\n", entry->d_name);
        nfiles++;
    }
    rewinddir(dir);

    s->items = malloc(nfiles * sizeof(st_t));
    s->hash.indexes = malloc(nfiles * 2 * sizeof(he_t));
    s->hash.hsize = nfiles * 2;

    nfiles = 0;
    while ((entry = readdir(dir)) != NULL)
    {
        char fullpath[1024];
        sprintf(fullpath, "%s/%s", BACKINGDIR, entry->d_name);
        if (stat(fullpath, &st) == 0)
        {
            if (S_ISREG(st.st_mode)) // If is a regular file
            {
                size_t cplen = strlen(entry->d_name) < sizeof(s->items[nfiles].fsname) ? strlen(entry->d_name) : sizeof(s->items[nfiles].fsname);
                memcpy(s->items[nfiles].fsname, entry->d_name, cplen);
                put_item(s, nfiles, st.st_size);
                nfiles++;
            }
        }
    }
    printf("Initialized cache for %ld objects\n", nfiles);

    closedir(dir);
}

int64_t get_item(s_t *s, char *name)
{
    u_int64_t fullhash = strtohash(name);
    off_t tindex = fullhash % s->hash.hsize;

    while (s->hash.indexes[tindex].fullhash && s->hash.indexes[tindex].fullhash != fullhash)
        tindex = (tindex + 1) % s->hash.hsize;

    if (s->hash.indexes[tindex].fullhash != fullhash)
        return -1;
    // cache_item(s, s->hash.indexes[tindex].index);
    return s->hash.indexes[tindex].index;
}

typedef struct my_threadparms
{
    int ssock;
    int csock;
    int idx;
    char type;
    s_t *st;
    char to[1024];
} thp;

void *copy_thread(void *vparams)
{
    thp *params = vparams;
    int ssock = params->ssock;
    s_t *st = params->st;
    char *to = params->to;
    // sem_wait(&sem);
    dprintf("Cache copy thread\n");

    // Tell client the mode
    ssize_t ss = send(ssock, "2", 1, 0);
    dprintf("Cache: wrote mode %s %d bytes to %d\n", "2", ss, ssock);

    while (1)
    {
        struct msghdr msg = {0};
        struct pollfd pfd;
        char m_buffer[256];
        dprintf("waiting on fd %d\n", ssock);

        pfd.fd = ssock;
        pfd.events = POLLERR | POLLHUP | POLLIN | POLLMSG;
        int pollr;
        while (pollr = poll(&pfd, 1, -1))
        {
            if (pollr < 0)
            {
                perror("pollfail");
                goto out;
            }
            if (pfd.revents & (POLLERR | POLLHUP))
            {
                goto out;
            }
            else
            {

                break;
            }
        }

        size_t recvbytes = read(ssock, m_buffer, sizeof(m_buffer));
        if (params->csock && fcntl(params->csock, F_GETFD) >= 0)
        {
            close(params->csock);
            params->csock = 0;
        }

        m_buffer[recvbytes] = '\0';
        dprintf("Read %d bytes from %d\n", recvbytes, ssock);
        dprintf("msg %s\n", m_buffer);
        if (recvbytes > 0)
        {
            char rrecname[1024] = {0};

            ssize_t reclen = recvbytes; // msg.msg_iov[0].iov_len;
            memcpy(rrecname, m_buffer, reclen);
            rrecname[reclen] = 0;
            int64_t idx = get_item(st, rrecname);
            if (idx >= 0)
            {
                int cfd = st->items[idx].cfd > 0 ? st->items[idx].cfd : st->items[idx].fsfd;
                ssize_t sendsize = st->items[idx].size;
                write(ssock, &sendsize, sizeof(sendsize));
                dprintf("Cache sent file size %ld (%d) to client\n", sendsize, sizeof(sendsize));
                ssize_t sfs, totsfs = 0;
                off_t offset = 0;
                while (1)
                {

                    sfs = sendfile(ssock, cfd, &offset, sendsize - offset);
                    if (sfs < 0)
                    {
                        printf("ret %ld ss %d cfd %d offset %ld sendsize %ld\n", ssock, cfd, offset, sendsize);
                        perror("sendfile");
                        break;
                    }
                    totsfs += sfs;
                    dprintf("send %d:%d byte file (%d) fd %d\n", sfs, totsfs, sendsize, cfd);
                    if (totsfs >= sendsize)
                        break;
                }
            }
            else
            {

                printf("no such object %s %d\n", rrecname, reclen);
            }
        }
    }
out:
    close(ssock);
    params->ssock = -1;
    printf("exit thread\n");
    sem_post(&sem);
    return (void *)params;
}

void *fdpass_thread(void *vparams)
{
    thp *params = vparams;
    int ssock = params->ssock;
    s_t *st = params->st;
    char *to = params->to;
    // sem_wait(&sem);
    dprintf("fdpass thread ssock %d csock %d\n", ssock, params->csock);
    send(ssock, "1", 1, 0);
    int fret;
    if (params->csock && (fret = fcntl(params->csock, F_GETFD)) >= 0)
    {
        close(params->csock);
        params->csock = 0;
    }
    while (1)
    {
        struct msghdr msg = {0};

        char m_buffer[25600];
        struct iovec io = {.iov_base = m_buffer, .iov_len = sizeof(m_buffer)};
        msg.msg_iov = &io;
        msg.msg_iovlen = 1;

        char c_buffer[256];
        msg.msg_control = c_buffer;
        msg.msg_controllen = sizeof(c_buffer);
        dprintf("waiting on fd %d %d tidx %d\n", ssock, params->ssock, params->idx);
        ssize_t recvbytes = recvmsg(ssock, &msg, 0);

        dprintf("Read %d bytes from %d\n", recvbytes, ssock);
        dprintf("msg.msg_len %d\n", msg.msg_iovlen);
        if (recvbytes > 0)
        {
            char rrecname[1024] = {0};
            char *recname = msg.msg_iov[0].iov_base;
            struct pollfd pfd;
            pfd.fd = ssock;
            pfd.events = POLLERR | POLLHUP | POLLIN | POLLMSG;
            int pollr;
#if 0
            while (pollr = poll(&pfd, 1, -1))
            {
                printf("%d %d\t", pollr, pfd.revents);
                sleep(1);
                if (pollr < 0)
                {
                    perror("pollfail");
                    goto out;
                }
                if (pfd.revents & (POLLERR | POLLHUP))
                {
                    goto out;
                }
                else
                {
                    printf("%d %d\t", pollr, pfd.revents);
                    break;
                }
            }

            printf("%d %d\t", pollr, pfd.revents);
            sleep(1);
#endif
            ssize_t reclen = recvbytes; // msg.msg_iov[0].iov_len;
            memcpy(rrecname, recname, reclen);
            rrecname[reclen] = 0;
            int64_t idx = get_item(st, rrecname);
            if (idx >= 0)
            {
                int cfd = st->items[idx].cfd > 0 ? st->items[idx].cfd : st->items[idx].fsfd;
                // cfd = st->items[idx].fsfd;
                //  make a cope and pass that
                if (0)
                {
                    int nfd = memfd_create("foo", 0);
                    ssize_t copysize = st->items[idx].size;
                    off_t ioff = 0, ooff = 0;
                    ssize_t cs = copy_file_range(cfd, &ioff, nfd,
                                                 &ooff,
                                                 copysize, 0);
                    if (cs < 0)
                    {
                        printf("cfr %ld bytes to %d\n", cs, nfd);
                        perror("cfr");
                        cs = sendfile(nfd, cfd,
                                      0,
                                      copysize);
                    }
                    if (cs != copysize)
                    {
                        printf("sendfile %ld bytes\n", cs);
                        perror("sendfile fail\n");
                    }
                }
                put_fd(ssock, NULL, cfd, OP_FDPASS);
                // close(cfd);
            }
            else
            {

                printf("no such object %s %d\n", rrecname, reclen);
            }
        }
        else
        {
            goto out;
        }
    }
out:
    if (fcntl(ssock, F_GETFD) >= 0)
        close(ssock);

    params->ssock = -1;
    dprintf("exit thread tidx %d\n", params->idx);
    sem_post(&sem);
    return (void *)params;
}

int main(int argc, char *argv[])
{
    s_t st;

    int nextthread = 0;
    char *sockpath = "/tmp/mycachesock";
    struct sockaddr_un sockname;
    int threadcount = 1;
    int c;
    while ((c = getopt(argc, argv, "t:")) != -1)
    {
        switch (c)
        {
        case 't':
            threadcount = atoi(optarg);
        }
    }
    pthread_t workers[threadcount];
    thp *tparms;
    tparms = calloc(threadcount, sizeof(*tparms));
    if (sem_init(&sem, 0, threadcount - 1) == -1)
    {
        perror("sem_init");
        exit(-1);
    }

    sockname.sun_family = AF_UNIX;
    strncpy(sockname.sun_path, (char *)sockpath, sizeof(sockname.sun_path));
    char mode = '2';

    init_storage(&st);

    int ssock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ssock < 0)
        return -1;
    unlink(sockpath);
    if (bind(ssock, (struct sockaddr *)&sockname, sizeof(struct sockaddr_un)))
    {
        perror("socket exists");
    }

    while (1)
    {
        char msg[80];
        struct sockaddr_un local, remote;
        // sem_wait(&sem);
        // sem_post(&sem);

        listen(ssock, 1);
        unsigned int sock_len = 0;
        struct sockaddr_un client_address;
        socklen_t cl_addrlen = sizeof(client_address);
        size_t recvbytes = recvfrom(ssock, &msg, 80, 0, &client_address, &cl_addrlen);
        printf("got %ld bytes %s %s\n", recvbytes, msg, client_address.sun_path);
        mode = msg[0];
        int *socks;
        socks = calloc(2 * threadcount, sizeof(int));
        if (recvbytes > 0)
        {
            // one req type on the main socket, we don't care

            if (mode == OP_COPY || mode == OP_SPLICE)
            {
                // Create a pipe and pass the other end to client, spawn a thread to run the server end on the pipe

                if (socketpair(AF_LOCAL, SOCK_STREAM, 0, &socks[2 * nextthread]) == 0)
                {
                    tparms[nextthread].ssock = socks[2 * nextthread];
                    tparms[nextthread].csock = socks[2 * nextthread + 1];
                    tparms[nextthread].type = mode;
                    tparms[nextthread].st = &st;
                    strncpy(tparms[nextthread].to, client_address.sun_path, strlen(client_address.sun_path));
                    dprintf("sockpair %d %d\n", socks[2 * nextthread], socks[2 * nextthread + 1]);
                    int pret = put_fd(ssock, client_address.sun_path, socks[2 * nextthread + 1], mode);
                    if (pret >= 0)
                    {
                        pthread_create(&workers[nextthread], 0, copy_thread, &tparms[nextthread]);
                        sem_wait(&sem);
                    }
                }
            }
            else
            {

                if (socketpair(AF_LOCAL, SOCK_SEQPACKET /*SOCK_DGRAM*/, 0, &socks[2 * nextthread]) == 0)
                {
                    tparms[nextthread].ssock = socks[2 * nextthread];
                    tparms[nextthread].csock = socks[2 * nextthread + 1];
                    tparms[nextthread].type = OP_FDPASS;
                    tparms[nextthread].idx = nextthread;
                    tparms[nextthread].st = &st;
                    strncpy(tparms[nextthread].to, client_address.sun_path, strlen(client_address.sun_path));
                    dprintf("sockpair %d %d\n", socks[2 * nextthread], socks[2 * nextthread + 1]);
                    dprintf("gonna put_fd %d (%d) via %d\n", socks[2 * nextthread + 1], nextthread, ssock);
                    int pret = put_fd(ssock, client_address.sun_path, socks[2 * nextthread + 1], mode);
                    if (pret >= 0)
                    {
                        dprintf("spawn thread %d, ssock %d\n", nextthread, tparms[nextthread].ssock);
                        pthread_create(&workers[nextthread], 0, fdpass_thread, &tparms[nextthread]);
                        sem_wait(&sem);
                    }
                }
            }
            // loop to next free thread, or wait for a thread to exit
            int iterations = 0;
            // do a semaphore wait in case all threads are in use
            // sem_wait(&sem);
            // sem_post(&sem);
            while (tparms[nextthread].ssock > 0)
            {
                nextthread = (nextthread + 1) % threadcount;
                // dprintf("nextthread %d", nextthread);
            }
            dprintf("nextthread %d", nextthread);
            if (tparms[nextthread].ssock == -1)
            {
                void *ret;
                dprintf("join %d\n", nextthread);
                pthread_join(workers[nextthread], &ret);
            }
        }
    }
    return 0;
}
