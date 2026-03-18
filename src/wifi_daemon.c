#include "proto/wifi_proto.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int signal;
    int secured;
    int saved;
    int connected;
} ap_info_t;

static volatile sig_atomic_t g_stop = 0;
static int g_wifi_enabled = 1;
static ap_info_t g_aps[] = {
    {"DashCam-Office-5G", 92, 1, 1, 1},
    {"DashCam-Guest", 78, 0, 0, 0},
    {"Home-WiFi", 66, 1, 1, 0},
    {"CoffeeShop_Free", 54, 0, 0, 0},
};

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void send_line(int fd, const char *line)
{
    (void)write(fd, line, strlen(line));
}

static void handle_scan_get(int fd)
{
    int i;
    char line[256];

    if (!g_wifi_enabled) {
        send_line(fd, "ERR\tWIFI_DISABLED\n");
        return;
    }
    send_line(fd, "OK\tSCAN\n");
    for (i = 0; i < (int)(sizeof(g_aps) / sizeof(g_aps[0])); i++) {
        snprintf(line, sizeof(line), "AP\t%s\t%d\t%d\t%d\t%d\n",
            g_aps[i].ssid, g_aps[i].signal, g_aps[i].secured, g_aps[i].saved, g_aps[i].connected);
        send_line(fd, line);
    }
    send_line(fd, "END\n");
}

static void handle_connect(int fd, char *ssid)
{
    int i;
    if (!ssid || ssid[0] == '\0') {
        send_line(fd, "ERR\tEINVAL\n");
        return;
    }
    for (i = 0; i < (int)(sizeof(g_aps) / sizeof(g_aps[0])); i++) {
        g_aps[i].connected = 0;
        if (strcmp(g_aps[i].ssid, ssid) == 0) {
            g_aps[i].connected = 1;
            g_aps[i].saved = 1;
            send_line(fd, "OK\tCONNECTED\n");
            return;
        }
    }
    send_line(fd, "ERR\tNOT_FOUND\n");
}

int main(void)
{
    int listen_fd;
    struct sockaddr_un addr;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    unlink(WIFI_SOCKET_PATH);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, WIFI_SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, 4) != 0) {
        perror("listen");
        close(listen_fd);
        unlink(WIFI_SOCKET_PATH);
        return 1;
    }

    while (!g_stop) {
        int cfd;
        char buf[256];
        ssize_t n;

        cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        n = read(cfd, buf, sizeof(buf) - 1);
        if (n > 0) {
            char *cmd;
            char *arg1;
            char *saveptr = NULL;

            buf[n] = '\0';
            cmd = strtok_r(buf, "\t\n", &saveptr);
            arg1 = strtok_r(NULL, "\t\n", &saveptr);

            if (cmd == NULL) {
                send_line(cfd, "ERR\tEINVAL\n");
            } else if (strcmp(cmd, "SET_ENABLED") == 0) {
                g_wifi_enabled = (arg1 && strcmp(arg1, "0") != 0) ? 1 : 0;
                send_line(cfd, "OK\tSTATE\n");
            } else if (strcmp(cmd, "SCAN_START") == 0) {
                send_line(cfd, "OK\tSCAN_STARTED\n");
            } else if (strcmp(cmd, "SCAN_GET") == 0) {
                handle_scan_get(cfd);
            } else if (strcmp(cmd, "CONNECT") == 0) {
                handle_connect(cfd, arg1);
            } else if (strcmp(cmd, "DISCONNECT") == 0) {
                int i;
                for (i = 0; i < (int)(sizeof(g_aps) / sizeof(g_aps[0])); i++) {
                    g_aps[i].connected = 0;
                }
                send_line(cfd, "OK\tDISCONNECTED\n");
            } else if (strcmp(cmd, "FORGET") == 0) {
                int i;
                for (i = 0; i < (int)(sizeof(g_aps) / sizeof(g_aps[0])); i++) {
                    if (arg1 && strcmp(g_aps[i].ssid, arg1) == 0) {
                        g_aps[i].saved = 0;
                        g_aps[i].connected = 0;
                    }
                }
                send_line(cfd, "OK\tFORGOT\n");
            } else {
                send_line(cfd, "ERR\tUNKNOWN_CMD\n");
            }
        }
        close(cfd);
    }

    close(listen_fd);
    unlink(WIFI_SOCKET_PATH);
    return 0;
}
