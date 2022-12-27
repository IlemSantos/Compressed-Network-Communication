#include "client.h"

int main(int argc, char *argv[]) {

    int portno;

    static struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"log", required_argument, NULL, 'l'},
        {"compress", no_argument, NULL, 'c'},
        {0, 0, 0, 0}};

    int opt;
    int portOpt = 0;
    int logOpt = 0;
    int log_fd = 0;
    int compressOpt = 0;

    while ((opt = getopt_long(argc, argv, "p:lc", options, NULL)) != -1) {
        switch (opt) {
        case 'p':
            portno = atoi(optarg);
            portOpt = 1;
            break;
        case 'l':
            logOpt = 1;
            log_fd = creat(optarg, S_IRWXU);
            if (log_fd < 0) {
                fprintf(stderr, "Unable to create log file. Error: %d, Message: %s\n", errno, strerror(errno));
                exit(1);
            }
            break;
        case 'c':
            compressOpt = 1;
            break;
        default:
            fprintf(stderr, "Incorrect argument: correct usage is ./client --port=portno [--log=pathname] [--compress]\n");
            exit(1);
        }
    }

    if (!portOpt) {
        fprintf(stderr, "Incorrect argument: correct usage is ./client --port=portno [--log=pathname] [--compress]\n");
        fprintf(stderr, "port not specified\n");
        exit(1);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    set_input_mode();

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
        error("ERROR opening socket");

    server = gethostbyname("localhost");
    if (server == NULL) {
        fprintf(stderr, "ERROR, no such host\n");
        exit(0);
    }

    memset((char *)&serv_addr, 0, sizeof(serv_addr)); // instead of memset use memset
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(portno);
    memcpy((char *)&serv_addr.sin_addr.s_addr, (char *)server->h_addr, server->h_length);

    if (connect(socket_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
        error("Error in establishing connection.\n");

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO;
    fds[0].events = POLLIN | POLLHUP | POLLERR;

    fds[1].fd = socket_fd;
    fds[1].events = POLLIN | POLLHUP | POLLERR;

    for (;;) {

        poll(fds, 2, 0);

        if (fds[0].revents & POLLIN) {
            int ret;
            ret = pipe_to_bash(STDIN_FILENO, socket_fd, compressOpt, logOpt, log_fd);
            if (ret != Z_OK)
                exit(ret);
        }

        if (fds[0].fd & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!\n");
            exit(1);
        }

        if (fds[1].revents & POLLIN) {
            int ret;
            ret = pipe_to_server(socket_fd, STDOUT_FILENO, compressOpt, logOpt, log_fd);
            if (ret != Z_OK)
                exit(ret);
        }

        if (fds[1].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!!\n");
            exit(1);
        }
    }

    return 0;
}
