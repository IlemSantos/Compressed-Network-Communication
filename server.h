#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
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
#include <zlib.h>

#define CHUNK 256

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

void sanitization(int __fd, const void *__buf, size_t __n)
{
    char *input = (char *)__buf;
    char carriage[2] = {'\r', '\n'};

    for (size_t i = 0; i < __n; i++) {
        char curr = input[i];
        switch (curr) {
        case '\r':
        case '\n':
            write(__fd, &carriage[1], sizeof(char));
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
            write(__fd, &curr, sizeof(char));
            break;
        }
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

int pipe_to_bash(int __fd1, int __fd2, int compressOpt) {

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
            fprintf(stderr, "ERROR reading from new_socket\n");
            inflateEnd(&infstream);
            return Z_ERRNO;
        }
        if (size == 0) {
            inflateEnd(&infstream);
            return Z_ERRNO;
            break;
        }

        if (!compressOpt) {
            have = size;
            ret = have != CHUNK ? Z_STREAM_END : Z_NO_FLUSH;
            sanitization(__fd2, in, have);
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
                // if (write(__fd2, out, have) != have) {
                //     inflateEnd(&infstream);
                //     // return Z_ERRNO;
                // }
                sanitization(__fd2, out, have);
            } while (infstream.avail_out == 0);
        }

        /* done when inflate() says it's done */
    } while (ret != Z_STREAM_END);

    /* clean up and return */
    inflateEnd(&infstream);
    return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

int pipe_to_server(int __fd1, int __fd2, int compressOpt) {

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
        flush = size != CHUNK ? Z_FINISH : Z_NO_FLUSH;

        if (!compressOpt) {
            have = size;
            if (send(__fd2, in, have, 0) != have) {
                deflateEnd(&defstream);
                return Z_ERRNO;
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

#endif // SERVER_H