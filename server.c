#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

// @see https://github.com/HardySimpson/zlog
#include "zlog.h"

// @see https://github.com/nodejs/http-parser
#include "http_parser.h"

#include "tools.h"

#define PORT 5001

// 逻辑处理
void handler(int sock);

// 信号处理
void sigchld_handler(int signal);

// 死循环
void forever_run(int listenSocketFd, struct sockaddr_in cli_addr);

// fork daemon process后续处理
void daemon_after();

// 使用
void usage(void);

// init zlog
zlog_category_t *y_zlog_init();

pid_t parent_pid;

/*
 * http-parser callbak
 */
int url_cb(http_parser *parser, const char *at, size_t length);
int header_field_cb(http_parser *parser, const char *p, size_t len);


// http parse setting
http_parser_settings settings;
static http_parser *parser;

int main(int argc, char *argv[]) {
    int listenSocketFd, portno;
    struct sockaddr_in serv_addr, cli_addr;
    pid_t pid;

    int daemon = 0;

    if (argc <= 1) {
        printf("The command had no other arguments.\n");
    } else if (argc > 1 && argc < 3) {
        int i;
        for (i = 1; i < argc; i++) {
            printf("argv[%d] = %s\n", i, argv[i]);
        }

        if (strcmp("-D", argv[1]) == 0) {
            printf("Server will run in daemon.");
            daemon = 1;
        }
    } else {
        usage();
    }

    /* First call to socket() function */
    listenSocketFd = socket(AF_INET, SOCK_STREAM, 0);

    if (listenSocketFd < 0) {
        perror("ERROR opening socket");
        exit(1);
    }
    /* Initialize socket structure */
    bzero((char *) &serv_addr, sizeof(serv_addr));
    portno = PORT;

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons((uint16_t) portno);

    /* Now bind the host address using bind() call.*/
    if (bind(listenSocketFd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        perror("ERROR on binding");
        exit(1);
    }

    listen(listenSocketFd, 5);


    // http-parser setting
    settings.on_url = url_cb;
    settings.on_header_field = header_field_cb;
    parser = malloc(sizeof(http_parser));
    http_parser_init(parser, HTTP_REQUEST);

    signal(SIGCHLD, sigchld_handler); // 防止子进程变成僵尸进程

    // daemon
    // @see http://www.netzmafia.de/skripten/unix/linux-daemon-howto.html
    if (daemon) {
        if ((pid = fork()) == 0) {
            /* Open any logs here */
            zlog_category_t *c;
            c = y_zlog_init();

            zlog_info(c, "hello, zlog");

            daemon_after();

            // parent pid
            parent_pid = getpid();
            printf("Parent pid is: %d\n", parent_pid);

            forever_run(listenSocketFd, cli_addr);
            zlog_fini();
        } else if (pid > 0) {
            printf("server pid is: %d\n", pid);
            close(listenSocketFd); // 引用计数-1
            zlog_fini();
            exit(EXIT_SUCCESS);
        } else {
            printf("Cannot daemonize\n");
            zlog_fini();
            exit(EXIT_FAILURE);
        }
    } else {
        // parent pid
        parent_pid = getpid();
        printf("Parent pid is: %d\n", parent_pid);

        forever_run(listenSocketFd, cli_addr);
        zlog_fini();
    }
}

void handler(int sock) {
    ssize_t recved, nparsed;
    char buffer[1024];
    bzero(buffer, 1024);
    recved = read(sock, buffer, 1023); // TODO why 1023?

    if (recved < 0) {
        perror("ERROR reading from socket");
        exit(1);
    }

    printf("Here is the message: %s\n", buffer);

    /* Start up / continue the parser.
     * Note we pass recved==0 to signal that EOF has been received.
     */
    nparsed = http_parser_execute(parser, &settings, buffer, strlen(buffer));

    printf("nparsed is: %d\n", (int) nparsed);
    printf("recved is: %d\n", (int) recved);

    if (parser->upgrade) { // websocket
        /* handle new protocol */
        // handle websocket
        printf("handle websocket here\n");
    } else if (nparsed != recved) {
        /* Handle error. Usually just close the connection. */
        perror("Handle error. Usually just close the connection");
        exit(1);
    } else { // http
        printf("status_code is: %d\n", parser->status_code);
        printf("http method is: %d\n", parser->method); // 1=GET

        char *reply;
        reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nI got your message";

        recved = write(sock, reply, strlen(reply));

        if (recved < 0) {
            perror("ERROR writing to socket");
            exit(1);
        }
    }
}

// @see https://chenjianlong.gitbooks.io/rfc-6455-websocket-protocol-in-chinese/content/section4/section4.html
void handshake() {
    const char *GID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
}

/* 处理僵尸进程 */
void sigchld_handler(int signal) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void forever_run(int listenSocketFd, struct sockaddr_in cli_addr) {
    int clientSocketFd, clilen;
    pid_t pid;
    clilen = sizeof(cli_addr);
    while (1) {
        clientSocketFd = accept(listenSocketFd, (struct sockaddr *) &cli_addr, (socklen_t *) &clilen);

        if (clientSocketFd < 0) {
            perror("ERROR on accept");
            exit(1);
        }

        /* Create child process */
        pid = fork();

        if (pid < 0) {
            perror("ERROR on fork");
            exit(1);
        }

        if (pid == 0) {
            parser->data = (void *) clientSocketFd;

            close(listenSocketFd); // 引用计数-1

            printf("Child processes are: ");
            get_all_child_process(parent_pid);

            handler(clientSocketFd);
            exit(0);
        } else {
            close(clientSocketFd);
        }
    } /* end of while */
}

void daemon_after() {
    pid_t sid;
    /* Change the file mode mask */
    umask(0);

    /* Create a new SID for the child process */
    sid = setsid();
    if (sid < 0) {
        /* Log any failure */
        exit(EXIT_FAILURE);
    }

    /* Change the current working directory */
    if ((chdir("/")) < 0) {
        /* Log any failure here */
        exit(EXIT_FAILURE);
    }

    /* Close out the standard file descriptors */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

// zlog init
zlog_category_t *y_zlog_init() {
    zlog_category_t *c;
    int rc;
    rc = zlog_init("/etc/zlog.conf");

    printf("rc is: %d\n", rc);
    if (rc) {
        printf("init failed\n");
        exit(-1);
    }
    c = zlog_get_category("my_cat");
    if (!c) {
        printf("get cat fail\n");
        zlog_fini();
        exit(-2);
    }
    return c;
}

void usage(void) {
    printf("Usage:\n");
    printf("-D run server in daemonize.\n");
    exit(8);
}


/*
 * http-parser callback
 */

int url_cb(http_parser *parser, const char *at, size_t len) {
    printf("url callback called\n");
    printf("at is: %s\n", at);
    printf("len is: %d\n", (int) len);
    /* access to thread local custom_data_t struct.
    Use this access save parsed data for later use into thread local
    buffer, or communicate over socket
    */
    return 0;
}

int header_field_cb(http_parser *parser, const char *p, size_t len) {
    printf("header field callback called\n");
    printf("p is: %s\n", p);
    printf("len is: %d\n", (int) len);
    return 0;
}
