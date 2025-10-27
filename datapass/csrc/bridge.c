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
#include <errno.h>
#include "sendget.h"

#define OP_FDPASS '1'
#define OP_COPY '2'
#define OP_SPLICE '3'

#ifdef DEBUG
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

typedef struct my_threadparms
{
    int ssockw; // sclient session socket
    int ssockr; // might be different from above in case of pipes
    int csock;  // spawned for client session socket, the 'other end'
    int usock;  // upstream server socket
    int dssock; // down client socket to server
    int idx;
    char type;

    char to[1024];
} thp;

sem_t sem;

void *copy_thread(void *vparams)
{
    thp *params = vparams;
    int ssockr = params->ssockr;
    int ssockw = params->ssockw;
    // int ofd = params->csock;
    int cdsock = params->dssock;

    // sem_wait(&sem);
    dprintf("copy thread\n");
    dprintf("ssock %d csock %d cddsock %dn", ssockr, params->csock, params->dssock);
    // Tell client the mode
    ssize_t ss = write(ssockw, "200", 3);

    dprintf("Wrote mode %s %d bytes to %d\n", "2", ss, ssockw);

    while (1)
    {
        struct msghdr msg = {0};
        struct pollfd pfd;
        char m_buffer[256];
        dprintf("waiting on fd %d\n", ssockr);

        size_t recvbytes = read(ssockr, m_buffer, sizeof(m_buffer));

        m_buffer[recvbytes] = '\0';
        dprintf("Read %d bytes from %d\n", recvbytes, ssockr);
        dprintf("msg %s\n", m_buffer);
        if (recvbytes > 0)
        {
            char rrecname[1024] = {0};

            ssize_t reclen = recvbytes; // msg.msg_iov[0].iov_len;
            memcpy(rrecname, m_buffer, reclen);
            rrecname[reclen] = 0;

            // send req down
            ssize_t wrbytes = write(cdsock, &m_buffer, reclen);
            // get the response size
            ssize_t smsgsz;
            ssize_t srb = read(cdsock, &smsgsz, sizeof(ssize_t));
            dprintf("Size of reply %ld, dtype %x stype %x\n", smsgsz, fcntl(ssockr, F_GETPIPE_SZ), fcntl(cdsock, F_GETPIPE_SZ));
            // get response / splice upstream
            ssize_t rb = 0;
            while (rb < sizeof(smsgsz))
            {
                ssize_t trb = write(ssockw, ((char *)&smsgsz) + rb, sizeof(smsgsz) - rb);
                if (trb < 0)
                {
                    perror("write failed");
                    goto out;
                }

                rb += trb;
                dprintf("wrote %ld of %ld bytes for size\n", trb, sizeof(smsgsz));
            }
            dprintf("Passed size up\n");
            ssize_t splicebytes = 0;
            while (splicebytes < smsgsz)
            {
                ssize_t sb;
                if (params->type == OP_SPLICE)
                {
                    dprintf("splice(%d, NULL, %d, NULL, %ld, 0)", cdsock, ssockw, smsgsz - splicebytes);
                    sb = splice(cdsock, NULL, ssockw, NULL, smsgsz - splicebytes, 0);
                    if (sb < 0)
                    {
                        perror("splice failed ");
                        goto out;
                    }
                    dprintf("spliced %ld bytes\n", sb);
                }
                else
                {
                    char rbuf[1024 * 1024];
                    off_t cbytes = smsgsz < sizeof(rbuf) ? smsgsz : sizeof(rbuf);
                    dprintf("read(%d, NULL, %d, NULL, %ld, 0)", cdsock, ssockw, cbytes - splicebytes);
                    ssize_t rb = read(cdsock, &rbuf, cbytes - splicebytes);
                    if (rb < 0)
                    {
                        perror("read failed ");
                        goto out;
                    }
                    ssize_t wb = 0, twb;
                    while (wb < rb)
                    {
                        twb = write(ssockw, &rbuf[wb], rb - wb);
                        if (twb < 0)
                        {
                            perror("write failed ");
                            goto out;
                        }
                        wb += twb;
                    }
                    dprintf("copied %ld bytes\n", wb);
                    sb = wb;
                }
                splicebytes += sb;
            }

#if 0
                mret = madvise(PAGE_ALIGN(rmem + advlen / 2), advlen / 2, MADV_SEQUENTIAL);
                if (mret)
                {
                    perror("SEQUENTIAL madvise");
                    exit(errno);
                }
#endif
        }
        else
        {
            goto out;
        }
    }
out:
    close(ssockr);
    close(ssockw);
    close(cdsock);
    params->ssockr = -1;
    params->ssockw = -1;
    printf("exit thread\n");
    sem_post(&sem);
    return (void *)params;
}

void *fdpass_thread(void *vparams)
{
    thp *params = vparams;
    int ssock = params->ssockr;
    int cdsock = params->dssock;
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
        dprintf("waiting on fd %d %d tidx %d\n", ssock, params->ssockr, params->idx);
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
            // send rec down
            struct msghdr smsg = {0};

            struct iovec sio;
            smsg.msg_iov = &sio;
            smsg.msg_iovlen = 1;
            ssize_t reclen = recvbytes; // msg.msg_iov[0].iov_len;
            memcpy(rrecname, recname, reclen);
            rrecname[reclen] = 0;
            sio.iov_base = rrecname;
            sio.iov_len = reclen;

            ssize_t wrbytes = sendmsg(cdsock, &smsg, 0);
            // get reaponse fd
            int ffd = get_fd(cdsock);
            // push fd up
            put_fd(ssock, NULL, ffd, OP_FDPASS);
            close(ffd);
        }
        else
        {
            goto out;
        }
    }
out:
    if (fcntl(ssock, F_GETFD) >= 0)
        close(ssock);
    close(cdsock);

    params->ssockr = -1;
    dprintf("exit thread tidx %d\n", params->idx);
    sem_post(&sem);
    return (void *)params;
}

int main(int argc, char *argv[])
{

    int nextthread = 0;
    char *sockpath = "/tmp/mybridgesock";
    char *csockpath = "/tmp/mycachesock";
    struct sockaddr_un sockname;
    struct sockaddr_un csau, cliaddr;
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
    if (sem_init(&sem, 0, threadcount) == -1)
    {
        perror("sem_init");
        exit(-1);
    }
    sockname.sun_family = AF_UNIX;
    strncpy(sockname.sun_path, (char *)sockpath, sizeof(sockname.sun_path));
    char mode = '2';

    int ssock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ssock < 0)
        return -1;
    unlink(sockpath);
    if (bind(ssock, (struct sockaddr *)&sockname, sizeof(struct sockaddr_un)))
    {
        perror("socket exists");
    }

    int csock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (csock < 0)
        return -1;

    csau.sun_family = AF_UNIX;
    strcpy(csau.sun_path, csockpath);
    size_t data_len = strlen(csau.sun_path) + sizeof(csau.sun_family);

    bzero(&cliaddr, sizeof(cliaddr)); /* bind an address for us */
    cliaddr.sun_family = AF_LOCAL;
    strcpy(cliaddr.sun_path, tmpnam(NULL));
    bind(csock, (struct sockaddr *)&cliaddr, sizeof(cliaddr));

    if (connect(csock, (struct sockaddr *)&csau, data_len))
    {
        printf("Cannot connect to server\n");
        return -1;
    }

    while (1)
    {
        char msg[80];
        struct sockaddr_un local, remote;
        sem_wait(&sem);
        sem_post(&sem);

        listen(ssock, 1);
        unsigned int sock_len = 0;
        struct sockaddr_un client_address;
        socklen_t cl_addrlen = sizeof(client_address);
        size_t recvbytes = recvfrom(ssock, &msg, 80, 0, &client_address, &cl_addrlen);
        dprintf("got %ld bytes %s %s\n", recvbytes, msg, client_address.sun_path);
        mode = msg[0];
        dprintf("Bridge: got mode %c\n", mode);
        int *socks, *rsocks;
        socks = calloc(2 * threadcount, sizeof(int));
        rsocks = calloc(2 * threadcount, sizeof(int));
        if (recvbytes > 0)
        {
            // request a sock from server to be passed on to the worker thread

            struct msghdr smsg = {0};
            char smodestr[2] = {mode, 0};

            struct iovec *sio = malloc(sizeof(struct iovec));
            sio->iov_base = smodestr;
            sio->iov_len = 1;

            smsg.msg_iov = sio;
            smsg.msg_iovlen = 1;

            size_t sendbytes = sendmsg(csock, &smsg, 0);
            dprintf("sent %ld\n", sendbytes);

            int lval = 0;
            u_int64_t macc = 0, tbytes = 0;
            if (sendbytes <= 0)
                exit errno;

            int cdsock = get_fd(csock);

            // read mode
            char rmode[2];
            dprintf("waiting on fd %d\n", cdsock);
            recv(cdsock, rmode, 1, 0);
            dprintf("server gives mode %c\n", rmode[0]);

            // one req type on the main socket, we don't care

            if (mode == OP_COPY || mode == OP_SPLICE)
            {

                // Create a pipe and pass the other end to client, spawn a thread to run the server end on the pipe

                if (pipe(&socks[2 * nextthread]) == 0 && pipe(&rsocks[2 * nextthread]) == 0)
                {
                    tparms[nextthread].ssockw = rsocks[2 * nextthread + 1];
                    tparms[nextthread].ssockr = socks[2 * nextthread];
                    tparms[nextthread].csock = socks[2 * nextthread + 1];

                    tparms[nextthread].dssock = cdsock;
                    tparms[nextthread].type = mode;

                    strncpy(tparms[nextthread].to, client_address.sun_path, strlen(client_address.sun_path));
                    dprintf("sockpair %d(%d) %d(%d)\n", rsocks[2 * nextthread], rsocks[2 * nextthread + 1], socks[2 * nextthread], socks[2 * nextthread + 1]);
                    int pret = put_fd(ssock, client_address.sun_path, rsocks[2 * nextthread], mode);
                    int rpret = put_fd(ssock, client_address.sun_path, socks[2 * nextthread + 1], mode);
                    close(rsocks[2 * nextthread]);
                    close(socks[2 * nextthread + 1]);
                    dprintf("Bridge: spawn copy_thread in mode %c\n", mode);
                    if (pret >= 0 && rpret >= 0)
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
                    tparms[nextthread].ssockw = socks[2 * nextthread];
                    tparms[nextthread].ssockr = socks[2 * nextthread];
                    tparms[nextthread].csock = socks[2 * nextthread + 1];
                    tparms[nextthread].dssock = cdsock;
                    tparms[nextthread].type = OP_FDPASS;
                    tparms[nextthread].idx = nextthread;

                    strncpy(tparms[nextthread].to, client_address.sun_path, strlen(client_address.sun_path));
                    dprintf("sockpair %d %d\n", socks[2 * nextthread], socks[2 * nextthread + 1]);
                    dprintf("gonna put_fd %d (%d) via %d\n", socks[2 * nextthread + 1], nextthread, ssock);
                    int pret = put_fd(ssock, client_address.sun_path, socks[2 * nextthread + 1], mode);
                    int rpret = put_fd(ssock, client_address.sun_path, socks[2 * nextthread + 1], mode);
                    if (pret >= 0 && rpret >= 0)
                    {
                        dprintf("spawn thread %d, ssock %d\n", nextthread, tparms[nextthread].ssockr);
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
            while (tparms[nextthread].ssockr > 0)
            {
                nextthread = (nextthread + 1) % threadcount;
                // dprintf("nextthread %d", nextthread);
            }
            dprintf("nextthread %d\n", nextthread);
            int ti = threadcount;
            while (ti--)
            {

                if (tparms[ti].ssockr == -1)
                {
                    void *ret;
                    printf("join %d\n", ti);
                    pthread_join(workers[ti], &ret);

                    tparms[ti].ssockr = 0;
                }
            }
        }
    }
    return 0;
}
