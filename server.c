#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define CHUNK     256
#define NO_FLUSH    0
#define FINISH      4

pid_t pid;
int fd0[2], fd1[2];
int socket_fd, new_socket;

void error(const char *string) {
    perror(string);
    exit(1);
}

void shutdown_socket() {
    close(new_socket);
    shutdown(socket_fd, SHUT_RDWR);
    close(socket_fd);
}

void sig_handler(int sig) {
    if (sig == SIGPIPE) {
        fprintf(stderr, "SIGPIPE received!\n");
        exit(0);
    }

    if (sig == SIGINT) {
        fprintf(stderr, "SIGINT received!\n");
        if (kill(pid, SIGINT) < 0) {
            fprintf(stderr, "Failed to kill process: Error:%d, Message: %s\n", errno, strerror(errno));
            exit(1);
        }
    }
}

int main(int argc, char *argv[]) {

    int portno, clilen;
    struct sockaddr_in serv_addr, cli_addr;

    struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {0, 0, 0, 0}};

    int opt;
    int portOpt = 0;

    while ((opt = getopt_long(argc, argv, "p:", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            portno = atoi(optarg);
            portOpt = 1;
            break;
        default:
            fprintf(stderr, "Incorrect argument: correct usage is ./server --port=portno\n");
            exit(1);
        }
    }

    if (!portOpt) {
        fprintf(stderr, "Incorrect argument: correct usage is ./server --port=portno\n");
        fprintf(stderr, "port not specified\n");
        exit(1);
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        error("ERROR opening socket");

    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(socket_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR on binding");

    if (listen(socket_fd, 5) < 0)
        error("ERROR while listening");

    clilen = sizeof(cli_addr);
    new_socket = accept(socket_fd, (struct sockaddr *)&cli_addr, (socklen_t *)&clilen);
    if (new_socket < 0)
        error("ERROR on accept");

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, sig_handler);

    pipe(fd0);
    pipe(fd1);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Fork failed\n");
        close(new_socket);
        shutdown(socket_fd, SHUT_RDWR);
        close(socket_fd);
        exit(1);
    }

    if (pid == 0) { // child
        close(fd0[1]);
        close(fd1[0]);

        dup2(fd0[0], STDIN_FILENO);
        close(fd0[0]); // already duplicated
        dup2(fd1[1], STDOUT_FILENO);
        dup2(fd1[1], STDERR_FILENO);
        close(fd1[1]); // already duplicated

        char *arguments[] = {"/bin/bash", (char *)NULL};
        execvp(arguments[0], arguments);
        fprintf(stderr, "ERROR in executing shell\n");
        exit(1);
    }
    else {
        close(fd0[0]);
        close(fd1[1]);

        atexit(shutdown_socket);

        struct pollfd fds[2];
        fds[0].fd = new_socket;
        fds[0].events = POLLIN | POLLHUP | POLLERR;

        fds[1].fd = fd1[0];
        fds[1].events = POLLIN | POLLHUP | POLLERR;

        char in[CHUNK + 1];
        // char out[CHUNK + 1];
        char carriage[2] = {'\r', '\n'};
        ssize_t ret;

        for (;;) {

            poll(fds, 2, 0);

            if (fds[0].revents & POLLIN) {
                memset(in, 0, CHUNK);
                ret = recv(new_socket, in, CHUNK, 0);
                if (ret < 0)
                    error("ERROR reading from new_socket");
                in[ret] = 0x00;

                for (int i = 0; i < ret; i++) {
                    char curr = in[i];
                    switch (curr) {
                    case '\r':
                    case '\n':
                        write(fd0[1], &carriage[1], sizeof(char));
                        break;
                    case 0x03:
                        fprintf(stderr, "SIGINT received\n");
                        if (kill(pid, SIGINT) < 0) {
                            fprintf(stderr, "Failed to kill process: Error:%d, Message: %s\n", errno, strerror(errno));
                            exit(1);
                        }
                        exit(1);
                        break;
                    case 0x04:
                        fprintf(stderr, "SIGPIPE received\n");
                        shutdown(new_socket, SHUT_RD);
                        close(fd0[1]);
                        // exit(0);
                        break;
                    default:
                        write(fd0[1], &curr, sizeof(char));
                        break;
                    }
                }
            }

            if (fds[0].revents & (POLLHUP | POLLERR)) {
                exit(0);
            }

            if (fds[1].revents & POLLIN) {
                int flush;
                do {
                    memset(in, 0, CHUNK);
                    ret = read(fd1[0], in, CHUNK);
                    if (ret < 0)
                        error("ERROR reading from pipe");
                    in[ret] = 0x00;
                    flush = ret != CHUNK ? FINISH : NO_FLUSH;

                    send(new_socket, in, ret, 0);
                } while (flush != FINISH);
            }

            if (fds[1].revents & (POLLHUP | POLLERR)) {
                // closing the connected socket
                shutdown(new_socket, SHUT_WR);
                exit(0);
            }
        }
    }

    return 0;
}