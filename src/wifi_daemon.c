#include <stddef.h>
#include "proto/wifi_proto.h"
#include "third_party/wpa_ctrl.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WLAN_CTRL_PATH "/var/run/wpa_supplicant/wlan0"
#define MAX_SCAN_LINES 128
#define MAX_NETWORK_LINES 128
#define BUF_SIZE 8192
#define CONNECT_TIMEOUT_SEC 20

typedef struct {
    int network_id;
    char ssid[WIFI_MAX_SSID_LEN];
    char flags[128];
} known_network_t;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    int signal;
    int secured;
    int saved;
    int connected;
} ap_info_t;

static volatile sig_atomic_t g_stop = 0;
static struct wpa_ctrl* g_ctrl = NULL;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void send_line(int fd, const char* line)
{
    if (line == NULL) {
        return;
    }
    (void)write(fd, line, strlen(line));
}

static int ensure_ctrl(void)
{
    if (g_ctrl != NULL) {
        return 0;
    }
    g_ctrl = wpa_ctrl_open(WLAN_CTRL_PATH);
    return (g_ctrl != NULL) ? 0 : -1;
}

static void close_ctrl(void)
{
    if (g_ctrl != NULL) {
        wpa_ctrl_close(g_ctrl);
        g_ctrl = NULL;
    }
}

static int run_cmd(const char* cmd, char* out, size_t out_sz)
{
    size_t len;
    int ret;

    if (cmd == NULL || out == NULL || out_sz < 2) {
        return -1;
    }
    if (ensure_ctrl() != 0) {
        return -1;
    }

    len = out_sz - 1;
    memset(out, 0, out_sz);
    ret = wpa_ctrl_request(g_ctrl, cmd, strlen(cmd), out, &len, NULL);
    if (ret != 0) {
        return -1;
    }
    out[len] = '\0';
    return 0;
}

static int is_protected(const char* flags)
{
    if (flags == NULL) {
        return 0;
    }
    return (strstr(flags, "WPA") != NULL) || (strstr(flags, "WEP") != NULL) || (strstr(flags, "SAE") != NULL);
}

static int parse_known_networks(known_network_t* list, int max_count)
{
    char buf[BUF_SIZE];
    char* saveptr = NULL;
    char* line;
    int count = 0;

    if (run_cmd("LIST_NETWORKS", buf, sizeof(buf)) != 0) {
        return 0;
    }

    line = strtok_r(buf, "\n", &saveptr); /* header */
    line = strtok_r(NULL, "\n", &saveptr);
    while (line != NULL && count < max_count) {
        int id = -1;
        char ssid[WIFI_MAX_SSID_LEN] = {0};
        char bssid[128] = {0};
        char flags[128] = {0};
        if (sscanf(line, "%d\t%63[^\t]\t%127[^\t]\t%127[^\n]", &id, ssid, bssid, flags) >= 2) {
            list[count].network_id = id;
            snprintf(list[count].ssid, sizeof(list[count].ssid), "%s", ssid);
            snprintf(list[count].flags, sizeof(list[count].flags), "%s", flags);
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return count;
}

static int find_network_id_by_ssid(const char* ssid, known_network_t* list, int list_count)
{
    int i;
    for (i = 0; i < list_count; i++) {
        if (strcmp(ssid, list[i].ssid) == 0) {
            return list[i].network_id;
        }
    }
    return -1;
}

static int wait_connected(const char* ssid)
{
    time_t start;
    char status[BUF_SIZE];

    start = time(NULL);
    while ((time(NULL) - start) < CONNECT_TIMEOUT_SEC) {
        if (run_cmd("STATUS", status, sizeof(status)) == 0) {
            if (strstr(status, "wpa_state=COMPLETED") && strstr(status, "ssid=") && strstr(status, ssid)) {
                return 0;
            }
        }
        usleep(200 * 1000);
    }
    return -1;
}

static void handle_set_enabled(int fd, const char* arg)
{
    int enable = (arg != NULL && strcmp(arg, "0") != 0) ? 1 : 0;
    int ret;

    if (enable) {
        ret = system("ifconfig wlan0 up");
        if (ret != 0) {
            send_line(fd, "ERR\tIF_UP\n");
            return;
        }
    } else {
        ret = system("ifconfig wlan0 down");
        if (ret != 0) {
            send_line(fd, "ERR\tIF_DOWN\n");
            return;
        }
        close_ctrl();
    }
    send_line(fd, "OK\tSTATE\n");
}

static void handle_scan_start(int fd)
{
    char out[BUF_SIZE];

    if (run_cmd("SCAN", out, sizeof(out)) != 0) {
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }
    if (strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }
    send_line(fd, "OK\tSCAN_STARTED\n");
}

static void handle_scan_get(int fd)
{
    char out[BUF_SIZE];
    known_network_t known[MAX_NETWORK_LINES];
    int known_count;
    char* saveptr = NULL;
    char* line;

    if (run_cmd("SCAN_RESULTS", out, sizeof(out)) != 0) {
        send_line(fd, "ERR\tSCAN_GET\n");
        return;
    }

    known_count = parse_known_networks(known, MAX_NETWORK_LINES);

    send_line(fd, "OK\tSCAN\n");
    line = strtok_r(out, "\n", &saveptr); /* header */
    line = strtok_r(NULL, "\n", &saveptr);
    while (line != NULL) {
        char bssid[64] = {0};
        int freq = 0;
        int signal = -100;
        char flags[128] = {0};
        char ssid[WIFI_MAX_SSID_LEN] = {0};
        int saved = 0;
        int connected = 0;
        int i;

        if (sscanf(line, "%63[^\t]\t%d\t%d\t%127[^\t]\t%63[^\n]", bssid, &freq, &signal, flags, ssid) >= 4) {
            if (ssid[0] != '\0') {
                for (i = 0; i < known_count; i++) {
                    if (strcmp(ssid, known[i].ssid) == 0) {
                        saved = 1;
                        if (strstr(known[i].flags, "[CURRENT]") != NULL) {
                            connected = 1;
                        }
                        break;
                    }
                }
                {
                    char out_line[256];
                    snprintf(out_line, sizeof(out_line), "AP\t%s\t%d\t%d\t%d\t%d\n", ssid, signal,
                             is_protected(flags) ? 1 : 0, saved, connected);
                    send_line(fd, out_line);
                }
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    send_line(fd, "END\n");
}

static void handle_disconnect(int fd)
{
    char out[BUF_SIZE];

    if (run_cmd("DISCONNECT", out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tDISCONNECT\n");
        return;
    }
    send_line(fd, "OK\tDISCONNECTED\n");
}

static void handle_forget(int fd, const char* ssid)
{
    known_network_t known[MAX_NETWORK_LINES];
    int id;
    int known_count;
    char cmd[128];
    char out[BUF_SIZE];

    if (ssid == NULL || ssid[0] == '\0') {
        send_line(fd, "ERR\tEINVAL\n");
        return;
    }

    known_count = parse_known_networks(known, MAX_NETWORK_LINES);
    id = find_network_id_by_ssid(ssid, known, known_count);
    if (id < 0) {
        send_line(fd, "ERR\tNOT_FOUND\n");
        return;
    }

    snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
    if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tFORGET\n");
        return;
    }
    (void)run_cmd("SAVE_CONFIG", out, sizeof(out));
    send_line(fd, "OK\tFORGOT\n");
}

static void handle_connect(int fd, const char* ssid, const char* password)
{
    known_network_t known[MAX_NETWORK_LINES];
    int known_count;
    int id;
    char cmd[256];
    char out[BUF_SIZE];

    if (ssid == NULL || ssid[0] == '\0') {
        send_line(fd, "ERR\tEINVAL\n");
        return;
    }

    known_count = parse_known_networks(known, MAX_NETWORK_LINES);
    id = find_network_id_by_ssid(ssid, known, known_count);

    if (id < 0) {
        if (run_cmd("ADD_NETWORK", out, sizeof(out)) != 0) {
            send_line(fd, "ERR\tADD_NETWORK\n");
            return;
        }
        id = atoi(out);

        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid \"%s\"", id, ssid);
        if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
            send_line(fd, "ERR\tSET_SSID\n");
            return;
        }

        if (password == NULL || password[0] == '\0') {
            snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
        } else {
            snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk \"%s\"", id, password);
        }
        if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
            send_line(fd, "ERR\tSET_PSK\n");
            return;
        }
    }

    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
    if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tSELECT_NETWORK\n");
        return;
    }

    (void)run_cmd("SAVE_CONFIG", out, sizeof(out));

    if (wait_connected(ssid) != 0) {
        send_line(fd, "ERR\tCONNECT_TIMEOUT\n");
        return;
    }

    send_line(fd, "OK\tCONNECTED\n");
}

static void handle_client(int cfd)
{
    char buf[512];
    ssize_t n;

    n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        return;
    }
    buf[n] = '\0';

    {
        char* saveptr = NULL;
        char* cmd = strtok_r(buf, "\t\n", &saveptr);
        char* arg1 = strtok_r(NULL, "\t\n", &saveptr);
        char* arg2 = strtok_r(NULL, "\t\n", &saveptr);

        if (cmd == NULL) {
            send_line(cfd, "ERR\tEINVAL\n");
        } else if (strcmp(cmd, "SET_ENABLED") == 0) {
            handle_set_enabled(cfd, arg1);
        } else if (strcmp(cmd, "SCAN_START") == 0) {
            handle_scan_start(cfd);
        } else if (strcmp(cmd, "SCAN_GET") == 0) {
            handle_scan_get(cfd);
        } else if (strcmp(cmd, "CONNECT") == 0) {
            handle_connect(cfd, arg1, arg2);
        } else if (strcmp(cmd, "DISCONNECT") == 0) {
            handle_disconnect(cfd);
        } else if (strcmp(cmd, "FORGET") == 0) {
            handle_forget(cfd, arg1);
        } else {
            send_line(cfd, "ERR\tUNKNOWN_CMD\n");
        }
    }
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
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", WIFI_SOCKET_PATH);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }
    if (listen(listen_fd, 8) != 0) {
        perror("listen");
        close(listen_fd);
        unlink(WIFI_SOCKET_PATH);
        return 1;
    }

    while (!g_stop) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        handle_client(cfd);
        close(cfd);
    }

    close_ctrl();
    close(listen_fd);
    unlink(WIFI_SOCKET_PATH);
    return 0;
}
