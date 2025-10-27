#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef DEBUG
#define dprintf(...) printf(__VA_ARGS__)
#else
#define dprintf(...)
#endif

int put_fd(int socket, char *to, int fd, char stype) // send fd by socket
{
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(fd))] = {0};
    struct sockaddr_un client_address;
    socklen_t cl_addrlen = sizeof(client_address);
    client_address.sun_family = AF_UNIX;

    memset(buf, '\0', sizeof(buf));
    struct iovec io = {.iov_base = &stype, .iov_len = 1};
    char *dest = to ? to : "N/A";

    dprintf("sending %d to %d mode %c dsock %s\n", fd, socket, stype, dest);
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);
    if (to != NULL)
    {
        strcpy(client_address.sun_path, to);

        msg.msg_name = &client_address;
        msg.msg_namelen = sizeof(client_address);
        dprintf("msgname '%s' '%ld'\n", msg.msg_name, msg.msg_namelen);
    }
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));

    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    msg.msg_controllen = CMSG_SPACE(sizeof(fd));
    ssize_t sb;
    // if (sb = send(socket, &tstmsg, sizeof(tstmsg), 0) < 0)
    if ((sb = sendmsg(socket, &msg, 0)) < 0)
    {
        printf("couldn't send fd %d to %d\n", fd, socket);
        perror("");
        abort();
        sleep(1);
        return -1;
    }
    dprintf("send fd %d to client on %d, %ld bytes\n", fd, socket, sb);
    return 0;
}

int get_fd(int socket) // receive fd from socket
{
    struct msghdr msg = {0};
    int fd;

    char m_buffer[256] = {0};
    struct iovec io = {.iov_base = m_buffer, .iov_len = sizeof(m_buffer)};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;

    char c_buffer[256] = {0};
    msg.msg_control = c_buffer;
    msg.msg_controllen = sizeof(c_buffer);
    ssize_t rb;
    while ((rb = recvmsg(socket, &msg, 0)) <= 0)
    {
        if (rb <= 0)
            return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

    unsigned char *data = CMSG_DATA(cmsg);

    memcpy(&fd, data, sizeof(fd));
    dprintf("got %d\n", fd);
    return fd;
}
