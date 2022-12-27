#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <assert.h>
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
#include <zlib.h>

#define CHUNK 256

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

int init_compress(z_streamp defstream)
{
    int ret;

    /* allocate deflate state */
    defstream->zalloc = Z_NULL;
    defstream->zfree = Z_NULL;
    defstream->opaque = Z_NULL;
    ret = deflateInit(defstream, Z_DEFAULT_COMPRESSION);
    if (ret != Z_OK)
        return ret;

    return Z_OK;
}

int init_uncompress(z_streamp infstream)
{
    int ret;

    /* allocate inflate state */
    infstream->zalloc = Z_NULL;
    infstream->zfree = Z_NULL;
    infstream->opaque = Z_NULL;
    infstream->avail_in = 0;
    infstream->next_in = Z_NULL;
    ret = inflateInit(infstream);
    if (ret != Z_OK)
        return ret;

    return Z_OK;
}

int pipe_to_bash(int __fd1, int __fd2, int compressOpt, int logOpt, int __log_fd) {

    int ret, size, flush;
    unsigned have;
    z_stream defstream;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    if (compressOpt) {
        ret = init_compress(&defstream);
        if (ret != Z_OK)
            return ret;
    }

    /* compress until end of file */
    do {
        memset(in, 0, CHUNK);
        size = read(__fd1, in, CHUNK);
        if (size < 0) {
            fprintf(stderr, "ERROR reading from pipe\n");
            deflateEnd(&defstream);
            return Z_ERRNO;
        }
        if (size == 0) {
            fprintf(stdout, "^D\r\n");
            int sigpipe = htonl(0x04);
            send(__fd2, &sigpipe, sizeof(sigpipe), 0);
        }
        flush = size != CHUNK ? Z_FINISH : Z_NO_FLUSH;

        if (!compressOpt) {
            have = size;
            if (send(__fd2, in, have, 0) != have) {
                deflateEnd(&defstream);
                return Z_ERRNO;
            }

            if (logOpt == 1) {
                dprintf(__log_fd, "SENT %d bytes: ", have);
                write(__log_fd, in, have);
                // dprintf(log_fd, &carriage[1], sizeof(char));
            }
        }
        else {
            defstream.avail_in = size;
            defstream.next_in = in;

            /* run deflate() on input until output buffer not full, finish
            compression if all of source has been read in */
            do {
                defstream.avail_out = CHUNK;
                defstream.next_out = out;
                ret = deflate(&defstream, flush); /* no bad return value */
                assert(ret != Z_STREAM_ERROR);    /* state not clobbered */
                have = CHUNK - defstream.avail_out;
                if (send(__fd2, out, have, 0) != have) {
                    deflateEnd(&defstream);
                    return Z_ERRNO;
                }
                if (logOpt == 1) {
                    dprintf(__log_fd, "SENT %d bytes: ", have);
                    write(__log_fd, out, have);
                    // dprintf(log_fd, &carriage[1], sizeof(char));
                }
            } while (defstream.avail_out == 0);
            assert(defstream.avail_in == 0); /* all input will be used */
        }

        /* done when last data in file processed */
    } while (flush != Z_FINISH);
    // assert(ret == Z_STREAM_END); /* stream will be complete */

    /* clean up and return */
    deflateEnd(&defstream);
    return Z_OK;
}

int pipe_to_server(int __fd1, int __fd2, int compressOpt, int logOpt, int __log_fd) {

    int ret, size;
    unsigned have;
    z_stream infstream;
    unsigned char in[CHUNK];
    unsigned char out[CHUNK];

    if (compressOpt) {
        ret = init_uncompress(&infstream);
        if (ret != Z_OK)
            return ret;
    }

    /* decompress until deflate stream ends or end of file */
    do {
        memset(in, 0, CHUNK);
        size = recv(__fd1, in, CHUNK, 0);
        if (size < 0) {
            fprintf(stderr, "ERROR reading from socket\n");
            inflateEnd(&infstream);
            return Z_ERRNO;
        }
        if (size == 0) {
            close(__fd1);
            exit(0);
            // break;
        }

        if (logOpt == 1) {
            dprintf(__log_fd, "RECEIVED %d bytes: ", size);
            write(__log_fd, in, size);
            // dprintf(log_fd, &carriage[1], sizeof(char));
        }

        if (!compressOpt) {
            have = size;
            ret = have != CHUNK ? Z_STREAM_END : Z_NO_FLUSH;
            if (write(__fd2, in, have) != have) {
                inflateEnd(&infstream);
                return Z_ERRNO;
            }
        }
        else {
            infstream.avail_in = size;
            infstream.next_in = in;

            /* run inflate() on input until output buffer not full */
            do {
                infstream.avail_out = CHUNK;
                infstream.next_out = out;
                ret = inflate(&infstream, Z_NO_FLUSH);
                assert(ret != Z_STREAM_ERROR); /* state not clobbered */
                switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR; /* and fall through */
                case Z_DATA_ERROR:
                case Z_MEM_ERROR:
                    inflateEnd(&infstream);
                    return ret;
                }
                have = CHUNK - infstream.avail_out;
                if (write(__fd2, out, have) != have) {
                    inflateEnd(&infstream);
                    return Z_ERRNO;
                }
            } while (infstream.avail_out == 0);
        }

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END);

    /* clean up and return */
    inflateEnd(&infstream);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

#endif // CLIENT_H