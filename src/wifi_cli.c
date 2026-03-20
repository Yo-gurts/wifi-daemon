#include "proto/wifi_proto.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CLI_BUF_SIZE 1024
#define CLI_MAX_TOKENS 16

static void trim_end(char* s)
{
    size_t len;

    if (s == NULL) {
        return;
    }

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static void print_usage(void)
{
    printf("wifi-cli usage:\n");
    printf("  wifi-cli                  # 交互模式\n");
    printf("  wifi-cli <cmd> [args...]  # 单次命令模式\n\n");
    printf("friendly commands:\n");
    printf("  help\n");
    printf("  status\n");
    printf("  enable <0|1>\n");
    printf("  scan\n");
    printf("  aps\n");
    printf("  connect <ssid> [password]\n");
    printf("  connect_result\n");
    printf("  disconnect\n");
    printf("  forget <ssid>\n");
    printf("  raw|RAW <daemon_command>\n");
    printf("  exit|quit\n\n");
    printf("raw daemon commands:\n");
    printf("  GET_STATUS\n");
    printf("  SET_ENABLED <0|1>\n");
    printf("  SCAN_START\n");
    printf("  SCAN_GET\n");
    printf("  CONNECT <ssid> [password]\n");
    printf("  GET_CONNECT_RESULT\n");
    printf("  DISCONNECT\n");
    printf("  FORGET <ssid>\n");
    printf("\n");
    printf("raw command examples:\n");
    printf("  GET_STATUS\n");
    printf("  SET_ENABLED\\t1\n");
    printf("  CONNECT\\tMyWiFi\\t12345678\n");
}

static int cmd_eq(const char* cmd, const char* expected)
{
    if (cmd == NULL || expected == NULL) {
        return 0;
    }
    return strcasecmp(cmd, expected) == 0;
}

static int parse_line(char* line, char* argv[], int max_tokens)
{
    int argc = 0;
    char* p = line;

    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (argc >= max_tokens) {
            return -1;
        }

        argv[argc++] = p;

        if (*p == '"') {
            char* dst;
            p++;
            argv[argc - 1] = p;
            dst = p;
            while (*p != '\0') {
                if (*p == '\\' && p[1] != '\0') {
                    p++;
                    *dst++ = *p++;
                    continue;
                }
                if (*p == '"') {
                    p++;
                    break;
                }
                *dst++ = *p++;
            }
            *dst = '\0';
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
        } else {
            while (*p != '\0' && !isspace((unsigned char)*p)) {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
        }
    }

    return argc;
}

static int send_request(const char* req)
{
    int fd;
    struct sockaddr_un addr;
    size_t left;
    const char* p;
    char resp[CLI_BUF_SIZE];

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WIFI_SOCKET_PATH);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "connect %s failed: %s\n", WIFI_SOCKET_PATH, strerror(errno));
        close(fd);
        return -1;
    }

    left = strlen(req);
    p = req;
    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "write failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }

    while (1) {
        ssize_t n = read(fd, resp, sizeof(resp) - 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "read failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        resp[n] = '\0';
        fputs(resp, stdout);
    }

    close(fd);
    return 0;
}

static int build_request_from_tokens(int argc, char* argv[], char* out, size_t out_sz)
{
    if (argc <= 0 || argv == NULL || out == NULL || out_sz == 0) {
        return -1;
    }

    if (cmd_eq(argv[0], "help")) {
        print_usage();
        return 1;
    }

    if (cmd_eq(argv[0], "status")) {
        return snprintf(out, out_sz, "GET_STATUS") > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "enable")) {
        if (argc < 2) {
            fprintf(stderr, "enable requires 0 or 1\n");
            return -1;
        }
        if (strcmp(argv[1], "0") != 0 && strcmp(argv[1], "1") != 0) {
            fprintf(stderr, "enable value must be 0 or 1\n");
            return -1;
        }
        return snprintf(out, out_sz, "SET_ENABLED\t%s", argv[1]) > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "scan")) {
        return snprintf(out, out_sz, "SCAN_START") > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "aps")) {
        return snprintf(out, out_sz, "SCAN_GET") > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "connect")) {
        if (argc < 2) {
            fprintf(stderr, "connect requires ssid\n");
            return -1;
        }
        if (argc >= 3) {
            return snprintf(out, out_sz, "CONNECT\t%s\t%s", argv[1], argv[2]) > 0 ? 0 : -1;
        }
        return snprintf(out, out_sz, "CONNECT\t%s", argv[1]) > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "connect_result")) {
        return snprintf(out, out_sz, "GET_CONNECT_RESULT") > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "disconnect")) {
        return snprintf(out, out_sz, "DISCONNECT") > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "forget")) {
        if (argc < 2) {
            fprintf(stderr, "forget requires ssid\n");
            return -1;
        }
        return snprintf(out, out_sz, "FORGET\t%s", argv[1]) > 0 ? 0 : -1;
    }

    if (cmd_eq(argv[0], "raw")) {
        int i;
        int off = 0;

        if (argc < 2) {
            fprintf(stderr, "raw requires daemon command\n");
            return -1;
        }

        for (i = 1; i < argc; i++) {
            int n = snprintf(out + off, out_sz - (size_t)off, "%s%s", (i == 1) ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= out_sz - (size_t)off) {
                return -1;
            }
            off += n;
        }
        return 0;
    }

    /* default: treat as raw daemon command */
    {
        int i;
        int off = 0;

        for (i = 0; i < argc; i++) {
            int n = snprintf(out + off, out_sz - (size_t)off, "%s%s", (i == 0) ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= out_sz - (size_t)off) {
                return -1;
            }
            off += n;
        }
    }

    return 0;
}

static int run_one_command(char* line)
{
    char* argv[CLI_MAX_TOKENS];
    char req[CLI_BUF_SIZE];
    int argc;
    int ret;

    trim_end(line);
    if (line[0] == '\0') {
        return 0;
    }

    argc = parse_line(line, argv, CLI_MAX_TOKENS);
    if (argc < 0) {
        fprintf(stderr, "too many arguments\n");
        return -1;
    }
    if (argc == 0) {
        return 0;
    }

    if (cmd_eq(argv[0], "exit") || cmd_eq(argv[0], "quit")) {
        return 1;
    }

    ret = build_request_from_tokens(argc, argv, req, sizeof(req));
    if (ret > 0) {
        return 0;
    }
    if (ret < 0) {
        return -1;
    }

    return send_request(req);
}

int main(int argc, char* argv[])
{
    if (argc > 1) {
        int i;
        char line[CLI_BUF_SIZE];
        int off = 0;

        for (i = 1; i < argc; i++) {
            int n = snprintf(line + off, sizeof(line) - (size_t)off, "%s%s", (i == 1) ? "" : " ", argv[i]);
            if (n < 0 || (size_t)n >= sizeof(line) - (size_t)off) {
                fprintf(stderr, "command too long\n");
                return 1;
            }
            off += n;
        }

        return run_one_command(line) == 0 ? 0 : 1;
    }

    print_usage();

    while (1) {
        char line[CLI_BUF_SIZE];
        int rc;

        printf("wifi-cli> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            putchar('\n');
            break;
        }

        rc = run_one_command(line);
        if (rc > 0) {
            break;
        }
    }

    return 0;
}
