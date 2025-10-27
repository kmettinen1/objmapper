/**
 * @file fdpass.h
 * @brief File descriptor passing over Unix domain sockets
 *
 * Provides a clean interface for sending and receiving file descriptors
 * between processes using Unix domain sockets and SCM_RIGHTS.
 */

#ifndef FDPASS_H
#define FDPASS_H

#include <sys/types.h>

/**
 * Send a file descriptor over a Unix domain socket
 * 
 * @param sock Socket file descriptor
 * @param dest_path Optional destination socket path (NULL for connected sockets)
 * @param fd File descriptor to send
 * @param operation_type Single character operation type indicator
 * @return 0 on success, -1 on failure
 */
int fdpass_send(int sock, const char *dest_path, int fd, char operation_type);

/**
 * Receive a file descriptor from a Unix domain socket
 * 
 * @param sock Socket file descriptor
 * @param operation_type Pointer to store the received operation type (can be NULL)
 * @return Received file descriptor on success, -1 on failure
 */
int fdpass_recv(int sock, char *operation_type);

#endif /* FDPASS_H */
