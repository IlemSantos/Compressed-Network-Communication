#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <getopt.h>
#include <poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netdb.h>

#define CHUNK     256
#define NO_FLUSH    0
#define FINISH      4

struct termios original_attributes;
struct termios new_attributes;

int socket_fd;
struct sockaddr_in serv_addr;
struct hostent *server;

void error(const char *string) {
    perror(string);
    exit(1);
}

void sig_handler(int sig) {
    if (sig == SIGPIPE) {
        // fprintf(stderr, "SIGPIPE received!!\n");
        exit(0);
    }

    if (sig == SIGINT) {
        // fprintf(stderr, "SIGINT received!!\n");
        int sigint = htonl(0x03);
        send(socket_fd, &sigint, sizeof(sigint), 0);
    }
}

void save_terminal_attributes() {
    int result = tcgetattr(STDIN_FILENO, &original_attributes);
    if (result < 0) {
        fprintf(stderr, "Error in getting attributes. Error: %d, Message: %s\n", errno, strerror(errno));
        exit(1);
    }
}

void reset() {
    if (tcsetattr(STDIN_FILENO, TCSANOW, &original_attributes) < 0) {
        fprintf(stderr, "Could not set the attributes\n");
        exit(1);
    }
}

void set_input_mode() {
    save_terminal_attributes();
    atexit(reset);

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, sig_handler);

    new_attributes = original_attributes;
    // new_attributes.c_lflag &= ~(ISIG);
    new_attributes.c_cc[VINTR] = 3; // ^C
    new_attributes.c_cc[VEOF] = 4;  // ^D
    int res = tcsetattr(STDIN_FILENO, TCSANOW, &new_attributes);
    if (res < 0) {
        fprintf(stderr, "Error with setting attributes. Error: %d, Message: %s\n", errno, strerror(errno));
        exit(1);
    }
}

int main(int argc, char *argv[]) {

    int portno;

    static struct option options[] = {
        {"port", required_argument, NULL, 'p'},
        {"log", required_argument, NULL, 'l'},
        {0, 0, 0, 0}};

    int opt;
    int portOpt = 0;
    int logOpt = 0;
    int log_fd = 0;

    while ((opt = getopt_long(argc, argv, "p:l", options, NULL)) != -1) {
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
        default:
            fprintf(stderr, "Incorrect argument: correct usage is ./client --port=portno [--log=pathname]\n");
            exit(1);
        }
    }

    if (!portOpt) {
        fprintf(stderr, "Incorrect argument: correct usage is ./client --port=portno [--log=pathname]\n");
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

    char in[CHUNK + 1];
    char out[CHUNK + 1];
    char carriage[2] = {'\r', '\n'};
    ssize_t ret;

    for (;;) {

        poll(fds, 2, 0);

        if (fds[0].revents & POLLIN) {
            memset(in, 0, CHUNK);
            ret = read(STDIN_FILENO, in, CHUNK);
            if (ret < 0)
                error("ERROR reading from stdin");
            in[ret] = 0x00;
            if (ret == 0) {
                fprintf(stdout, "^D\r\n");
                int sigpipe = htonl(0x04);
                send(socket_fd, &sigpipe, sizeof(sigpipe), 0);
            }

            for (int i = 0; i < ret; i++) {
                char curr = in[i];
                switch (curr) {
                case '\r':
                case '\n':
                    write(STDOUT_FILENO, &carriage[0], sizeof(char));
                    out[i] = carriage[1];
                    break;
                default:
                    out[i] = curr;
                    break;
                }
            }

            send(socket_fd, out, ret, 0);

            if (logOpt == 1) {
                dprintf(log_fd, "SENT %ld bytes: ", ret);
                write(log_fd, out, ret);
                // dprintf(log_fd, &carriage[1], sizeof(char));
            }
        }

        if (fds[0].fd & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!\n");
            exit(1);
        }

        if (fds[1].revents & POLLIN) {
            int flush;
            do {
                memset(in, 0, CHUNK);
                ret = recv(socket_fd, in, CHUNK, 0);
                if (ret < 0)
                    error("ERROR reading from socket");
                in[ret] = 0x00;
                if (ret == 0) {
                    close(socket_fd);
                    exit(0);
                }
                flush = ret != CHUNK ? FINISH : NO_FLUSH;

                if (logOpt == 1) {
                    dprintf(log_fd, "RECEIVED %ld bytes: ", ret);
                    write(log_fd, in, ret);
                }

                for (int i = 0; i < ret; i++) {
                    char curr = in[i];
                    switch (curr) {
                    case '\r':
                    case '\n':
                        write(STDOUT_FILENO, &carriage[0], sizeof(char));
                        write(STDOUT_FILENO, &carriage[1], sizeof(char));
                        break;
                    default:
                        write(STDOUT_FILENO, &curr, sizeof(char));
                        break;
                    }
                }
            } while (flush != FINISH);
        }

        if (fds[1].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "Server shut down!!\n");
            exit(1);
        }
    }

    return 0;
}
