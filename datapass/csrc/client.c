#include <fcntl.h>
#include <ctype.h>
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
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include "sendget.h"

#define BACKINGDIR "./back"

#ifdef DEBUG
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

typedef struct fnlist
{
    off_t nfiles;
    char *filenames;
    u_int64_t *filecounts;
} fl_t;

char *getobjlist(fl_t *fls)
{
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
        char fullpath[1024];
        sprintf(fullpath, "%s/%s", BACKINGDIR, entry->d_name);
        if (stat(fullpath, &st) == 0)
        {
            if (S_ISREG(st.st_mode)) // If is a regular file
            {
                // printf("%s\n", entry->d_name);
                nfiles++;
            }
        }
    }
    rewinddir(dir);
    char *retlist = malloc((nfiles + 1) * 1024);
    u_int64_t *counts = calloc(nfiles + 1, sizeof(u_int64_t));
    char *sptr = retlist;
    nfiles = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        char fullpath[1024];
        sprintf(fullpath, "%s/%s", BACKINGDIR, entry->d_name);
        if (stat(fullpath, &st) == 0)
        {
            if (S_ISREG(st.st_mode) && strlen(entry->d_name) > 1) // If is a regular file
            {

                sprintf(sptr, "%s", entry->d_name);
                dprintf("%s %s %s %p\n", entry->d_name, &retlist[nfiles * 1024], sptr, sptr);
                sptr = retlist + (nfiles++ * 1024);
            }
        }
    }
    nfiles--;
    fls->filenames = retlist;
    fls->filecounts = counts;
    fls->nfiles = nfiles;
    printf("%d files to request\n", nfiles);
}

#define SOCKPATH "/tmp/mybridgesock"

int main(int argc, char *argv[])
{
    fl_t fl;
    char mode = '1';
    struct sockaddr_un sau, cliaddr;
    int c;
    int wr = 0;
    int randidx = 0;
    off_t pindex = 0, stride = 64;
    u_int64_t rthresh = 0;

    size_t reqcount;
    while ((c = getopt(argc, argv, "CSc:t:ws:r")) != -1)
    {
        switch (c)
        {
        case 'C':
            mode = '2';
            break;
        case 'S':
            mode = '3';
            break;
        case 'c':
            reqcount = atoi(optarg);
            break;
        case 's':
            stride = atoi(optarg);
            break;

        case 't':
            rthresh = atoi(optarg);
            break;
        case 'w':
            wr = 1;
            break;
        case 'r':
            randidx = 1;
            break;
        }
    }

    getobjlist(&fl);

    int ssock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ssock < 0)
        return -1;

    sau.sun_family = AF_UNIX;
    strcpy(sau.sun_path, SOCKPATH);
    size_t data_len = strlen(sau.sun_path) + sizeof(sau.sun_family);

    bzero(&cliaddr, sizeof(cliaddr)); /* bind an address for us */
    cliaddr.sun_family = AF_LOCAL;
    strcpy(cliaddr.sun_path, tmpnam(NULL));
    bind(ssock, (struct sockaddr *)&cliaddr, sizeof(cliaddr));

    if (connect(ssock, (struct sockaddr *)&sau, data_len))
    {
        printf("Cannot connect to server\n");
        return -1;
    }

    struct msghdr msg = {0};
    char modestr[2] = {mode, 0};

    struct iovec *io = malloc(sizeof(struct iovec));
    io->iov_base = modestr;
    io->iov_len = 1;

    msg.msg_iov = io;
    msg.msg_iovlen = 1;
    struct timespec ts1, ts2;
    struct timespec ttfb1, ttfb2;
    u_int64_t tttfbsum = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    printf("%ld.%09ld\n", (long)(ts2.tv_sec - ts1.tv_sec),
           ts2.tv_nsec - ts1.tv_nsec);
    srand(ts1.tv_nsec);
    size_t sendbytes = sendmsg(ssock, &msg, 0);
    printf("sent %ld\n", sendbytes);
    u_int64_t inreqs = reqcount;
    int lval = 0;
    u_int64_t macc = 0, tbytes = 0;
    if (sendbytes > 0)
    {
        int ofdr = get_fd(ssock);
        int ofdw = get_fd(ssock);

        clock_gettime(CLOCK_MONOTONIC, &ts1);
        dprintf("Got fds %d %d\n", ofdr, ofdw);
        // read mode
        char mode[20];
        struct pollfd rfs[2];
        dprintf("waiting on fd %d\n", ofdr);
        rfs[0].fd = ofdr;
        rfs[0].revents = POLLIN;
        rfs[1].fd = ofdw;
        rfs[1].revents = POLLIN;
        // int pret = poll(&rfs, 2, -1);
        // dprintf("pret %d, revents %x, revents %x\n", pret, rfs[0].revents, rfs[1].revents);
        ssize_t mbytes = read(ofdr, mode, 20);
        dprintf("server gives mode %c (%d)\n", mode[0], mbytes);
        if (mbytes < 0)
        {
            perror("couldn't read");
        }

        close(ssock);
        int current = 0;
        u_int64_t nreqs = inreqs;

        if (mode[0] == '1')
        {
            while (1)
            {
                // pick a random obj and request it and touch all the pages

                io->iov_base = &fl.filenames[current * 1024];
                io->iov_len = strlen(&fl.filenames[current * 1024]);
                if (strstr(&fl.filenames[current * 1024], "bfilu4"))
                {
                    pindex = current;
                    dprintf("set index at %ld\n", pindex);
                }
                clock_gettime(CLOCK_MONOTONIC, &ttfb1);
                ssize_t wrbytes = sendmsg(ofdw, &msg, 0);
                dprintf("wrote %ld bytes of %ld %s %s to %d idx %d %p\n", wrbytes, io->iov_len, io->iov_base, &fl.filenames[current * 1024], ofdw, current, &fl.filenames[current * 1024]);
                int ffd = get_fd(ofdr);
                if (ffd < 0)
                {
                    perror("get_fd");
                    return -1;
                }
                dprintf("Got reqfile as %d\n", ffd);
                off_t fsize = lseek(ffd, 0, SEEK_END);

                dprintf("received file size %ld, total %ld\n", fsize, tbytes);
                int mflags = MAP_PRIVATE; // | MAP_NORESERVE | MAP_LOCKED; // | MAP_POPULATE;
                // mflags = MAP_PRIVATE | MAP_NONBLOCK | MAP_HUGE;
                int mprot = PROT_READ;
                if (wr)
                    mprot |= PROT_WRITE;
                size_t mlen = fsize;
                size_t advlen = (fsize * rthresh / 100);
                char *rmem = mmap(NULL, advlen /*mlen*/, mprot, mflags, ffd, 0);
                if (rmem == -1)
                {

                    perror("can't mmap");
                }
                int mret = madvise(rmem, advlen, MADV_RANDOM);
                if (mret)
                {
                    perror("RANDOM madvise");
                    exit(errno);
                }
#if 0
                mret = madvise(PAGE_ALIGN(rmem + advlen / 2), advlen / 2, MADV_SEQUENTIAL);
                if (mret)
                {
                    perror("SEQUENTIAL madvise");
                    exit(errno);
                }
#endif
                int *acc = rmem;
                tbytes += fsize;
                dprintf("acc %p rmem %p rmem_fsize %p\n", acc, rmem, rmem + fsize);
                u_int64_t tsum = 0, totread = 0;
                while (acc < (rmem + fsize) && totread < (fsize * rthresh / 100))
                {
                    lval += *acc;
                    tsum += *acc;
                    if (acc == rmem)
                    {
                        clock_gettime(CLOCK_MONOTONIC, &ttfb2);
                        u_int64_t tttime = ((ttfb2.tv_sec - ttfb1.tv_sec) * 1000000) + ((ttfb2.tv_nsec - ttfb1.tv_nsec) / 1000);
                        tttfbsum += tttime;
                    }
                    if (wr)
                    {
                        *acc = tsum;
                    }
                    // if (current == pindex)
                    //     printf("%ld %p %ld %d\t", *acc, acc, tsum, current);
                    // acc++;
                    acc += stride;
                    macc++;
                    totread = (void *)acc - (void *)rmem;

                    // dprintf("macc %ld\n", macc);
                }

                munmap(rmem, fsize * rthresh / 100);
                close(ffd);
                fl.filecounts[current] = tsum;
                if (randidx)
                {
                    current = (current + 1) % fl.nfiles;
                }
                else
                {
                    current = rand() % fl.nfiles;
                }
                nreqs--;

                if (!nreqs)
                    break;
            }
        }
        if (mode[0] == '2' || mode[0] == '3')
        {
            while (1)
            {
                // pick a random obj and request it and touch all the pagesFor streaming, egt size first
                char request[1024] = {0};
                sprintf(request, "%s", &fl.filenames[current * 1024]);
                clock_gettime(CLOCK_MONOTONIC, &ttfb1);
                ssize_t wrbytes = write(ofdw, &request, strlen(request));
                dprintf("wrote %ld bytes %s idx %d %p\n", wrbytes, request, current, &fl.filenames[current * 1024]);

                ssize_t smsgsz;
                ssize_t srb = read(ofdr, &smsgsz, sizeof(ssize_t));
                clock_gettime(CLOCK_MONOTONIC, &ttfb2);
                u_int64_t tttime = ((ttfb2.tv_sec - ttfb1.tv_sec) * 1000000) + ((ttfb2.tv_nsec - ttfb1.tv_nsec) / 1000);
                tttfbsum += tttime;
                ssize_t rb = 0;
                dprintf("read %d bytes for size, size=%d\n", srb, smsgsz);
                u_int64_t tsum = 0, totread = 0;

                while (rb < smsgsz)
                {
                    char buf[1024 * 1024];
                    ssize_t rbn;

                    rbn = read(ofdr, &buf, 1024 * 1024);
                    dprintf("read %d of %d bytes\n", rbn, smsgsz);
                    if (rbn > 0)
                    {
                        off_t i = 0;
                        while (i < rbn && (rb + i) < (smsgsz * rthresh / 100))
                        {
                            tsum += buf[i];
                            if (wr)
                                buf[i] = tsum;
                            i += stride * sizeof(int);
                            dprintf("i %d rb %ld rbn %ld szmsg %ld bthresg %d increment %d stride %d\n", i, rb, rbn, smsgsz, (smsgsz * rthresh / 100), stride * sizeof(int), stride);
                            macc++;
                        }
                        rb += rbn;
                        totread += rbn;
                    }
                    else
                    {
                        break;
                    }
                }

                tbytes += smsgsz;
                // fl.filecounts[current]++;
                fl.filecounts[current] = tsum;
                if (randidx)
                {
                    current = (current + 1) % fl.nfiles;
                }
                else
                {
                    current = rand() % fl.nfiles;
                }

                nreqs--;
                if (!nreqs)
                    break;
            }
        }
    }
    else
    {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts2);
    if (ts2.tv_nsec < ts1.tv_nsec)
    {
        ts2.tv_nsec += 1000000000;
        ts2.tv_sec--;
    }
    float rtime = (float)(ts2.tv_sec - ts1.tv_sec) + (float)((ts2.tv_nsec - ts1.tv_nsec) / 1000000000.0);

    printf("%f seconds %f files/sec %ld bytes %ld touches first file sum %ld name %s pindex %d, ttfbtot %lu inreqs %ld ttfbavg %lu %d touches/iteration\n", rtime, inreqs / rtime, tbytes, macc, fl.filecounts[pindex], &fl.filenames[pindex * 1024], pindex, tttfbsum, inreqs, tttfbsum / inreqs, macc / inreqs);

    return 0;
}
