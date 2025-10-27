/**
 * @file fdpass.c
 * @brief Implementation of file descriptor passing
 */

#define _GNU_SOURCE
#include "fdpass.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef DEBUG
#define dprintf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dprintf(...) ((void)0)
#endif

int fdpass_send(int sock, const char *dest_path, int fd, char operation_type)
{
    struct msghdr msg = {0};
    char buf[CMSG_SPACE(sizeof(fd))];
    struct sockaddr_un dest_addr;
    struct iovec io = {.iov_base = &operation_type, .iov_len = 1};

    memset(buf, 0, sizeof(buf));
    
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    /* Set destination address if provided */
    if (dest_path != NULL) {
        memset(&dest_addr, 0, sizeof(dest_addr));
        dest_addr.sun_family = AF_UNIX;
        strncpy(dest_addr.sun_path, dest_path, sizeof(dest_addr.sun_path) - 1);
        msg.msg_name = &dest_addr;
        msg.msg_namelen = sizeof(dest_addr);
        dprintf("fdpass_send: dest='%s'\n", dest_path);
    }

    /* Attach file descriptor to control message */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

    msg.msg_controllen = CMSG_SPACE(sizeof(fd));

    dprintf("fdpass_send: fd=%d, sock=%d, type=%c\n", fd, sock, operation_type);

    ssize_t sent = sendmsg(sock, &msg, 0);
    if (sent < 0) {
        perror("fdpass_send: sendmsg failed");
        return -1;
    }

    dprintf("fdpass_send: sent %zd bytes\n", sent);
    return 0;
}

int fdpass_recv(int sock, char *operation_type)
{
    struct msghdr msg = {0};
    char m_buffer[256];
    char c_buffer[256];
    struct iovec io = {.iov_base = m_buffer, .iov_len = sizeof(m_buffer)};
    int fd = -1;

    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = c_buffer;
    msg.msg_controllen = sizeof(c_buffer);

    ssize_t received = recvmsg(sock, &msg, 0);
    if (received <= 0) {
        if (received < 0) {
            perror("fdpass_recv: recvmsg failed");
        }
        return -1;
    }

    /* Extract operation type if requested */
    if (operation_type != NULL && received > 0) {
        *operation_type = m_buffer[0];
    }

    /* Extract file descriptor from control message */
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg != NULL && cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
        dprintf("fdpass_recv: received fd=%d, type=%c\n", fd, 
                operation_type ? *operation_type : '?');
    }

    return fd;
}
