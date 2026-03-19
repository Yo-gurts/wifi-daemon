#include <stddef.h>
#include "proto/wifi_proto.h"
#include "third_party/wpa_ctrl.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
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

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char bssid[64];
    int freq;
    int signal;
    char flags[128];
} ap_entry_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_enabled = 0;  /* wifi enabled flag */
static struct wpa_ctrl* g_ctrl = NULL;
static struct wpa_ctrl* g_ctrl_ev = NULL;
static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ctrl_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_scan_cache[BUF_SIZE];
static int g_scan_valid = 0;
static int g_scan_id = 0;  /* increment each scan, returned to client */

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
    pthread_mutex_lock(&g_ctrl_mutex);
    if (g_ctrl != NULL) {
        pthread_mutex_unlock(&g_ctrl_mutex);
        return 0;
    }
    g_ctrl = wpa_ctrl_open(WLAN_CTRL_PATH);
    if (g_ctrl == NULL) {
        pthread_mutex_unlock(&g_ctrl_mutex);
        return -1;
    }
    /* open event monitor connection */
    g_ctrl_ev = wpa_ctrl_open2(WLAN_CTRL_PATH, NULL);
    if (g_ctrl_ev == NULL) {
        wpa_ctrl_close(g_ctrl);
        g_ctrl = NULL;
        pthread_mutex_unlock(&g_ctrl_mutex);
        return -1;
    }
    if (wpa_ctrl_attach(g_ctrl_ev) != 0) {
        wpa_ctrl_close(g_ctrl_ev);
        wpa_ctrl_close(g_ctrl);
        g_ctrl = NULL;
        g_ctrl_ev = NULL;
        pthread_mutex_unlock(&g_ctrl_mutex);
        return -1;
    }
    pthread_mutex_unlock(&g_ctrl_mutex);
    return 0;
}

static void close_ctrl(void)
{
    pthread_mutex_lock(&g_ctrl_mutex);
    g_enabled = 0;
    if (g_ctrl != NULL) {
        wpa_ctrl_close(g_ctrl);
        g_ctrl = NULL;
    }
    if (g_ctrl_ev != NULL) {
        wpa_ctrl_detach(g_ctrl_ev);
        wpa_ctrl_close(g_ctrl_ev);
        g_ctrl_ev = NULL;
    }
    pthread_mutex_unlock(&g_ctrl_mutex);
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

static void update_scan_cache(void)
{
    char buf[BUF_SIZE];
    size_t len = sizeof(buf) - 1;

    if (g_ctrl == NULL) {
        return;
    }

    memset(buf, 0, sizeof(buf));
    if (wpa_ctrl_request(g_ctrl, "SCAN_RESULTS", 12, buf, &len, NULL) == 0) {
        buf[len] = '\0';
        pthread_mutex_lock(&g_scan_mutex);
        snprintf(g_scan_cache, sizeof(g_scan_cache), "%s", buf);
        g_scan_valid = 1;
        pthread_mutex_unlock(&g_scan_mutex);
    }
}

static void* event_thread(void* arg)
{
    (void)arg;
    char buf[BUF_SIZE];
    size_t len;

    while (!g_stop && g_enabled) {
        pthread_mutex_lock(&g_ctrl_mutex);
        if (g_ctrl_ev == NULL || !g_enabled) {
            pthread_mutex_unlock(&g_ctrl_mutex);
            usleep(100000);
            continue;
        }

        int fd = wpa_ctrl_get_fd(g_ctrl_ev);
        pthread_mutex_unlock(&g_ctrl_mutex);

        if (fd < 0) {
            usleep(100000);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {1, 0};

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (!g_enabled) {
            break;
        }
        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            pthread_mutex_lock(&g_ctrl_mutex);
            if (g_ctrl_ev != NULL) {
                len = sizeof(buf) - 1;
                memset(buf, 0, sizeof(buf));
                if (wpa_ctrl_recv(g_ctrl_ev, buf, &len) == 0) {
                    buf[len] = '\0';
                    /* check for scan results event */
                    if (strstr(buf, "CTRL-EVENT-SCAN-RESULTS") != NULL) {
                        update_scan_cache();
                    }
                }
            }
            pthread_mutex_unlock(&g_ctrl_mutex);
        }
    }
    return NULL;
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
        g_enabled = 1;
        /* ensure ctrl is opened */
        if (ensure_ctrl() != 0) {
            send_line(fd, "ERR\tCTRL_OPEN\n");
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
    char resp[64];

    if (run_cmd("SCAN", out, sizeof(out)) != 0) {
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }
    if (strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }
    /* increment scan_id and invalidate cache */
    g_scan_id++;
    pthread_mutex_lock(&g_scan_mutex);
    g_scan_valid = 0;
    pthread_mutex_unlock(&g_scan_mutex);
    snprintf(resp, sizeof(resp), "OK\tSCAN_STARTED\t%d\n", g_scan_id);
    send_line(fd, resp);
}

static int ap_cmp(const void *a, const void *b)
{
    const ap_entry_t *pa = (const ap_entry_t *)a;
    const ap_entry_t *pb = (const ap_entry_t *)b;
    /* descending order by signal strength */
    return pb->signal - pa->signal;
}

static void handle_scan_get(int fd)
{
    known_network_t known[MAX_NETWORK_LINES];
    int known_count;
    char resp[64];
    int scan_id;
    ap_entry_t aps[MAX_SCAN_LINES];
    int ap_count = 0;

    pthread_mutex_lock(&g_scan_mutex);
    scan_id = g_scan_id;
    if (!g_scan_valid) {
        pthread_mutex_unlock(&g_scan_mutex);
        /* no scan results yet */
        snprintf(resp, sizeof(resp), "OK\tSCAN\t%d\n", scan_id);
        send_line(fd, resp);
        send_line(fd, "END\n");
        return;
    }
    /* copy cache under lock */
    char scan_buf[BUF_SIZE];
    snprintf(scan_buf, sizeof(scan_buf), "%s", g_scan_cache);
    pthread_mutex_unlock(&g_scan_mutex);

    known_count = parse_known_networks(known, MAX_NETWORK_LINES);

    /* parse all entries */
    char *saveptr;
    char *line = strtok_r(scan_buf, "\n", &saveptr);
    line = strtok_r(NULL, "\n", &saveptr); /* skip header */
    while (line != NULL && ap_count < MAX_SCAN_LINES) {
        ap_entry_t *ap = &aps[ap_count];
        if (sscanf(line, "%63[^\t]\t%d\t%d\t%127[^\t]\t%63[^\n]",
                   ap->bssid, &ap->freq, &ap->signal, ap->flags, ap->ssid) >= 4) {
            if (ap->ssid[0] != '\0') {
                ap_count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* sort by signal strength descending */
    qsort(aps, ap_count, sizeof(ap_entry_t), ap_cmp);

    snprintf(resp, sizeof(resp), "OK\tSCAN\t%d\n", scan_id);
    send_line(fd, resp);

    /* send sorted entries */
    for (int i = 0; i < ap_count; i++) {
        ap_entry_t *ap = &aps[i];
        int saved = 0;
        int connected = 0;
        for (int j = 0; j < known_count; j++) {
            if (strcmp(ap->ssid, known[j].ssid) == 0) {
                saved = 1;
                if (strstr(known[j].flags, "[CURRENT]") != NULL) {
                    connected = 1;
                }
                break;
            }
        }
        char out_line[512];
        snprintf(out_line, sizeof(out_line), "AP\t%.63s\t%d\t%d\t%d\t%d\n",
            ap->ssid, ap->signal,
            is_protected(ap->flags) ? 1 : 0, saved, connected);
        send_line(fd, out_line);
    }
    send_line(fd, "END\n");
}

static void handle_get_status(int fd)
{
    char resp[64];
    snprintf(resp, sizeof(resp), "OK\tSTATUS\t%d\n", g_enabled);
    send_line(fd, resp);
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
        } else if (strcmp(cmd, "GET_STATUS") == 0) {
            handle_get_status(cfd);
        } else {
            send_line(cfd, "ERR\tUNKNOWN_CMD\n");
        }
    }
}

int main(void)
{
    int listen_fd;
    struct sockaddr_un addr;
    pthread_t ev_tid;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* start event listener thread */
    pthread_create(&ev_tid, NULL, event_thread, NULL);

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
