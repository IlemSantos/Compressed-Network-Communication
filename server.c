#include "server.h"

int main(int argc, char *argv[])
{

    int portno, clilen;
    struct sockaddr_in serv_addr, cli_addr;

    struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"compress", no_argument, NULL, 'c'},
        {0, 0, 0, 0}};

    int opt;
    int portOpt = 0;
    int compressOpt = 0;

    while ((opt = getopt_long(argc, argv, "p:c", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            portno = atoi(optarg);
            portOpt = 1;
            break;
        case 'c':
            compressOpt = 1;
            break;
        default:
            fprintf(stderr, "Incorrect argument: correct usage is ./server --port=portno [--compress]\n");
            exit(1);
        }
    }

    if (!portOpt) {
        fprintf(stderr, "Incorrect argument: correct usage is ./server --port=portno [--compress]\n");
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

        for (;;) {

            poll(fds, 2, 0);

            if (fds[0].revents & POLLIN) {
                int ret;
                ret = pipe_to_bash(new_socket, fd0[1], compressOpt);
                if (ret != Z_OK)
                    exit(ret);
            }

            if (fds[0].revents & (POLLHUP | POLLERR)) {
                exit(0);
            }

            if (fds[1].revents & POLLIN) {
                int ret;
                ret = pipe_to_server(fd1[0], new_socket, compressOpt);
                if (ret != Z_OK)
                    exit(ret);
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
