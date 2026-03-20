#include "mlog.h"
#include "proto/wifi_proto.h"
#include "third_party/wpa_ctrl.h"
#include <stddef.h>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WLAN_CTRL_PATH "/var/run/wpa_supplicant/wlan0"
#define WPA_CTRL_CLI_PATH_CMD "/tmp/wpa_ctrl_wifi_daemon_cmd"
#define WPA_CTRL_CLI_PATH_EVT "/tmp/wpa_ctrl_wifi_daemon_evt"
#define WPA_CTRL_CLI_PATH_WAIT "/tmp/wpa_ctrl_wifi_daemon_wait"
#define MAX_SCAN_LINES 128
#define MAX_NETWORK_LINES 128
#define BUF_SIZE 8192
#define CONNECT_TIMEOUT_SEC 12

typedef enum {
    CONNECT_WAIT_OK = 0,
    CONNECT_WAIT_TIMEOUT = -1,
    CONNECT_WAIT_AUTH_FAILED = -2
} connect_wait_result_t;

typedef enum {
    CONNECT_STATE_IDLE = 0,
    CONNECT_STATE_CONNECTING = 1,
    CONNECT_STATE_CONNECTED = 2,
    CONNECT_STATE_FAILED = 3
} connect_state_t;

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
    int saved;
    int connected;
    char flags[128];
} ap_entry_t;

typedef struct {
    char ssid[WIFI_MAX_SSID_LEN];
    char password[WIFI_MAX_PASSWORD_LEN];
} connect_task_t;

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_enabled = 0;  /* wifi enabled flag */
static struct wpa_ctrl* g_ctrl = NULL;
static struct wpa_ctrl* g_ctrl_ev = NULL;
static pthread_mutex_t g_scan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ctrl_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_connect_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_scan_cache[BUF_SIZE];
static int g_scan_valid = 0;
static int g_scan_id = 0;  /* increment each scan, returned to client */
static int g_connect_worker_running = 0;
static connect_state_t g_connect_state = CONNECT_STATE_IDLE;
static char g_connect_error[64] = "NONE";
static char g_connect_ssid[WIFI_MAX_SSID_LEN] = "";

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void send_line(int fd, const char* line)
{
    const char* p;
    size_t left;

    if (line == NULL) {
        return;
    }
    p = line;
    left = strlen(line);
    while (left > 0) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (n == 0) {
            return;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
}

static void set_connect_status(connect_state_t state, const char* ssid, const char* error)
{
    pthread_mutex_lock(&g_connect_mutex);
    g_connect_state = state;
    if (ssid != NULL) {
        snprintf(g_connect_ssid, sizeof(g_connect_ssid), "%s", ssid);
    }
    if (error != NULL) {
        snprintf(g_connect_error, sizeof(g_connect_error), "%s", error);
    }
    pthread_mutex_unlock(&g_connect_mutex);
}

static int try_start_connect_worker(const char* ssid)
{
    int busy = 0;

    pthread_mutex_lock(&g_connect_mutex);
    if (g_connect_worker_running) {
        busy = 1;
    } else {
        g_connect_worker_running = 1;
        g_connect_state = CONNECT_STATE_CONNECTING;
        snprintf(g_connect_ssid, sizeof(g_connect_ssid), "%s", ssid);
        snprintf(g_connect_error, sizeof(g_connect_error), "%s", "NONE");
    }
    pthread_mutex_unlock(&g_connect_mutex);

    return busy ? -1 : 0;
}

static void finish_connect_worker(connect_state_t state, const char* error)
{
    pthread_mutex_lock(&g_connect_mutex);
    g_connect_worker_running = 0;
    g_connect_state = state;
    snprintf(g_connect_error, sizeof(g_connect_error), "%s", (error != NULL) ? error : "NONE");
    pthread_mutex_unlock(&g_connect_mutex);
}

static int ensure_ctrl(void)
{
    pthread_mutex_lock(&g_ctrl_mutex);
    if (g_ctrl != NULL) {
        pthread_mutex_unlock(&g_ctrl_mutex);
        return 0;
    }
    g_ctrl = wpa_ctrl_open2(WLAN_CTRL_PATH, WPA_CTRL_CLI_PATH_CMD);
    if (g_ctrl == NULL) {
        pthread_mutex_unlock(&g_ctrl_mutex);
        return -1;
    }
    /* open event monitor connection */
    g_ctrl_ev = wpa_ctrl_open2(WLAN_CTRL_PATH, WPA_CTRL_CLI_PATH_EVT);
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
    g_scan_valid = 0;
    pthread_mutex_unlock(&g_ctrl_mutex);

    set_connect_status(CONNECT_STATE_IDLE, "", "NONE");
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

    pthread_mutex_lock(&g_ctrl_mutex);
    if (g_ctrl == NULL) {
        pthread_mutex_unlock(&g_ctrl_mutex);
        return -1;
    }
    len = out_sz - 1;
    memset(out, 0, out_sz);
    ret = wpa_ctrl_request(g_ctrl, cmd, strlen(cmd), out, &len, NULL);
    pthread_mutex_unlock(&g_ctrl_mutex);
    if (ret != 0) {
        return -1;
    }
    out[len] = '\0';
    return 0;
}

static void update_scan_cache(void)
{
    char out[BUF_SIZE];

    if (run_cmd("SCAN_RESULTS", out, sizeof(out)) == 0) {
        pthread_mutex_lock(&g_scan_mutex);
        snprintf(g_scan_cache, sizeof(g_scan_cache), "%s", out);
        g_scan_valid = 1;
        pthread_mutex_unlock(&g_scan_mutex);
    }
}

static void* event_thread(void* arg)
{
    (void)arg;
    char buf[BUF_SIZE];
    size_t len;

    while (!g_stop) {
        pthread_mutex_lock(&g_ctrl_mutex);
        if (g_ctrl_ev == NULL || !g_enabled) {
            /* If enabled but no ctrl connection, try to reconnect */
            if (g_enabled && g_ctrl_ev == NULL) {
                if (g_ctrl == NULL) {
                    g_ctrl = wpa_ctrl_open2(WLAN_CTRL_PATH, WPA_CTRL_CLI_PATH_CMD);
                }
                if (g_ctrl != NULL) {
                    g_ctrl_ev = wpa_ctrl_open2(WLAN_CTRL_PATH, WPA_CTRL_CLI_PATH_EVT);
                    if (g_ctrl_ev != NULL && wpa_ctrl_attach(g_ctrl_ev) != 0) {
                        wpa_ctrl_close(g_ctrl_ev);
                        g_ctrl_ev = NULL;
                    }
                }
            }
            pthread_mutex_unlock(&g_ctrl_mutex);
            usleep(100000);
            continue;
        }

        int fd = wpa_ctrl_get_fd(g_ctrl_ev);
        pthread_mutex_unlock(&g_ctrl_mutex);

        if (fd < 0) {
            /* connection broken, try to reconnect */
            pthread_mutex_lock(&g_ctrl_mutex);
            if (g_ctrl_ev != NULL) {
                wpa_ctrl_detach(g_ctrl_ev);
                wpa_ctrl_close(g_ctrl_ev);
                g_ctrl_ev = NULL;
            }
            pthread_mutex_unlock(&g_ctrl_mutex);
            usleep(100000);
            continue;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = {1, 0};

        int ret = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (!g_enabled) {
            continue;
        }
        if (ret > 0 && FD_ISSET(fd, &rfds)) {
            int scan_ready = 0;
            pthread_mutex_lock(&g_ctrl_mutex);
            if (g_ctrl_ev != NULL) {
                len = sizeof(buf) - 1;
                memset(buf, 0, sizeof(buf));
                if (wpa_ctrl_recv(g_ctrl_ev, buf, &len) == 0) {
                    buf[len] = '\0';
                    /* check for scan results event */
                    if (strstr(buf, "CTRL-EVENT-SCAN-RESULTS") != NULL) {
                        scan_ready = 1;
                    }
                }
            }
            pthread_mutex_unlock(&g_ctrl_mutex);
            if (scan_ready) {
                update_scan_cache();
            }
        }
    }
    MLOG_INFO("WIFI-DAEMON stoped!!!");
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

static int is_auth_failed_event(const char* ev)
{
    if (ev == NULL) {
        return 0;
    }

    if (strstr(ev, "reason=WRONG_KEY") != NULL) {
        return 1;
    }
    if (strstr(ev, "pre-shared key may be incorrect") != NULL) {
        return 1;
    }
    return 0;
}

static int is_connected_from_status(const char* status)
{
    char local[BUF_SIZE];
    char* saveptr = NULL;
    char* line;
    int completed = 0;

    if (status == NULL || status[0] == '\0') {
        return 0;
    }

    snprintf(local, sizeof(local), "%s", status);
    line = strtok_r(local, "\n", &saveptr);
    while (line != NULL) {
        if (strcmp(line, "wpa_state=COMPLETED") == 0) {
            completed = 1;
            break;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return completed;
}

static int get_rssi_dbm(void)
{
    char out[BUF_SIZE];
    char local[BUF_SIZE];
    char* saveptr = NULL;
    char* line;

    if (run_cmd("SIGNAL_POLL", out, sizeof(out)) != 0) {
        return -1;
    }

    snprintf(local, sizeof(local), "%s", out);
    line = strtok_r(local, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "RSSI=", 5) == 0) {
            return atoi(line + 5);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return -1;
}

static int wait_connected(const char* ssid)
{
    time_t start;
    char status[BUF_SIZE];
    struct wpa_ctrl* ctrl_ev_local = NULL;

    start = time(NULL);
    ctrl_ev_local = wpa_ctrl_open2(WLAN_CTRL_PATH, WPA_CTRL_CLI_PATH_WAIT);
    if (ctrl_ev_local != NULL) {
        if (wpa_ctrl_attach(ctrl_ev_local) != 0) {
            wpa_ctrl_close(ctrl_ev_local);
            ctrl_ev_local = NULL;
        }
    }

    while ((time(NULL) - start) < CONNECT_TIMEOUT_SEC) {
        if (ctrl_ev_local != NULL) {
            int ev_fd = wpa_ctrl_get_fd(ctrl_ev_local);
            if (ev_fd >= 0) {
                fd_set rfds;
                struct timeval tv = { 0, 100000 };
                FD_ZERO(&rfds);
                FD_SET(ev_fd, &rfds);
                if (select(ev_fd + 1, &rfds, NULL, NULL, &tv) > 0 && FD_ISSET(ev_fd, &rfds)) {
                    size_t ev_len = sizeof(status) - 1;
                    memset(status, 0, sizeof(status));
                    if (wpa_ctrl_recv(ctrl_ev_local, status, &ev_len) == 0) {
                        status[ev_len] = '\0';
                        if (is_auth_failed_event(status)) {
                            wpa_ctrl_detach(ctrl_ev_local);
                            wpa_ctrl_close(ctrl_ev_local);
                            return CONNECT_WAIT_AUTH_FAILED;
                        }
                    }
                }
            }
        }

        if (run_cmd("STATUS", status, sizeof(status)) == 0) {
            int completed = 0;
            int ssid_match = 0;
            char* saveptr = NULL;
            char* line = strtok_r(status, "\n", &saveptr);
            while (line != NULL) {
                if (strcmp(line, "wpa_state=COMPLETED") == 0) {
                    completed = 1;
                } else if (strncmp(line, "ssid=", 5) == 0 && strcmp(line + 5, ssid) == 0) {
                    ssid_match = 1;
                }
                line = strtok_r(NULL, "\n", &saveptr);
            }
            if (completed && ssid_match) {
                if (ctrl_ev_local != NULL) {
                    wpa_ctrl_detach(ctrl_ev_local);
                    wpa_ctrl_close(ctrl_ev_local);
                }
                return 0;
            }
        }
        usleep(200 * 1000);
    }

    if (ctrl_ev_local != NULL) {
        wpa_ctrl_detach(ctrl_ev_local);
        wpa_ctrl_close(ctrl_ev_local);
    }
    return CONNECT_WAIT_TIMEOUT;
}

static void forget_network_id(int id)
{
    char cmd[64];
    char out[BUF_SIZE];

    if (id < 0) {
        return;
    }

    snprintf(cmd, sizeof(cmd), "REMOVE_NETWORK %d", id);
    if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        MLOG_ERR("REMOVE_NETWORK failed for id=%d", id);
        return;
    }
    (void)run_cmd("SAVE_CONFIG", out, sizeof(out));
    MLOG_INFO("forgot network id=%d after auth failure", id);
}

static int quote_wpa_value(const char* in, char* out, size_t out_sz)
{
    size_t i;
    size_t pos = 0;

    if (in == NULL || out == NULL || out_sz < 3) {
        return -1;
    }

    out[pos++] = '"';
    for (i = 0; in[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)in[i];
        if (ch < 0x20 || ch == 0x7f) {
            return -1;
        }
        if (ch == '"' || ch == '\\') {
            if (pos + 2 >= out_sz) {
                return -1;
            }
            out[pos++] = '\\';
            out[pos++] = (char)ch;
            continue;
        }
        if (pos + 1 >= out_sz) {
            return -1;
        }
        out[pos++] = (char)ch;
    }

    if (pos + 2 > out_sz) {
        return -1;
    }
    out[pos++] = '"';
    out[pos] = '\0';
    return 0;
}

static void handle_set_enabled(int fd, const char* arg)
{
    int enable = (arg != NULL && strcmp(arg, "0") != 0) ? 1 : 0;
    int ret;

    MLOG_INFO("SET_ENABLED: %d", enable);
    if (enable) {
        ret = system("ifconfig wlan0 up");
        if (ret != 0) {
            MLOG_ERR("ifconfig wlan0 up failed: %d", ret);
            send_line(fd, "ERR\tIF_UP\n");
            return;
        }
        /* give wpa_supplicant time to initialize after interface up */
        usleep(500000);
        g_enabled = 1;
        /* ensure ctrl is opened */
        if (ensure_ctrl() != 0) {
            MLOG_ERR("ensure_ctrl failed");
            send_line(fd, "ERR\tCTRL_OPEN\n");
            return;
        }
        MLOG_INFO("wifi enabled");
    } else {
        ret = system("ifconfig wlan0 down");
        if (ret != 0) {
            MLOG_ERR("ifconfig wlan0 down failed: %d", ret);
            send_line(fd, "ERR\tIF_DOWN\n");
            return;
        }
        close_ctrl();
        MLOG_INFO("wifi disabled");
    }
    send_line(fd, "OK\tSTATE\n");
}

static void handle_scan_start(int fd)
{
    char out[BUF_SIZE];
    char resp[64];
    int retry = 3;

    MLOG_INFO("SCAN_START");
    while (retry-- > 0) {
        if (run_cmd("SCAN", out, sizeof(out)) != 0) {
            MLOG_ERR("SCAN command failed");
            send_line(fd, "ERR\tSCAN_START\n");
            return;
        }
        if (strstr(out, "OK") != NULL) {
            break;
        }
        if (strstr(out, "FAIL-BUSY") != NULL) {
            MLOG_WARN("SCAN busy, retrying...");
            usleep(500000);
            continue;
        }
        MLOG_ERR("SCAN command not OK: %s", out);
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }

    if (retry < 0 || strstr(out, "OK") == NULL) {
        MLOG_ERR("SCAN failed after retries");
        send_line(fd, "ERR\tSCAN_START\n");
        return;
    }

    pthread_mutex_lock(&g_scan_mutex);
    g_scan_id++;
    g_scan_valid = 0;
    snprintf(resp, sizeof(resp), "OK\tSCAN_STARTED\t%d\n", g_scan_id);
    pthread_mutex_unlock(&g_scan_mutex);

    MLOG_INFO("scan started async, scan_id=%d", g_scan_id);
    send_line(fd, resp);
}

static int ap_cmp(const void *a, const void *b)
{
    const ap_entry_t* pa = (const ap_entry_t*)a;
    const ap_entry_t* pb = (const ap_entry_t*)b;
    if (pb->connected != pa->connected) {
        return pb->connected - pa->connected;
    }
    /* descending order by signal strength */
    return pb->signal - pa->signal;
}

static int get_current_ssid(char* out, size_t out_sz)
{
    char status[BUF_SIZE];
    char* saveptr = NULL;
    char* line;

    if (out == NULL || out_sz == 0) {
        return -1;
    }
    out[0] = '\0';

    if (run_cmd("STATUS", status, sizeof(status)) != 0) {
        return -1;
    }

    line = strtok_r(status, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "ssid=", 5) == 0) {
            snprintf(out, out_sz, "%s", line + 5);
            return 0;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }
    return -1;
}

static void handle_scan_get(int fd)
{
    known_network_t known[MAX_NETWORK_LINES];
    char current_ssid[WIFI_MAX_SSID_LEN];
    int known_count;
    char resp[64];
    int scan_id;
    char scan_buf[BUF_SIZE];
    ap_entry_t aps[MAX_SCAN_LINES];
    int ap_count = 0;

    pthread_mutex_lock(&g_scan_mutex);
    scan_id = g_scan_id;
    if (!g_scan_valid) {
        pthread_mutex_unlock(&g_scan_mutex);
        /* Fallback: actively pull SCAN_RESULTS in case event was missed. */
        update_scan_cache();
        pthread_mutex_lock(&g_scan_mutex);
        scan_id = g_scan_id;
        if (!g_scan_valid) {
            pthread_mutex_unlock(&g_scan_mutex);
            MLOG_INFO("SCAN_GET: scan_id=%d, cache not ready", scan_id);
            snprintf(resp, sizeof(resp), "OK\tSCAN\t%d\n", scan_id);
            send_line(fd, resp);
            send_line(fd, "END\n");
            return;
        }
    }
    /* copy cache under lock */
    snprintf(scan_buf, sizeof(scan_buf), "%s", g_scan_cache);
    pthread_mutex_unlock(&g_scan_mutex);

    known_count = parse_known_networks(known, MAX_NETWORK_LINES);
    if (get_current_ssid(current_ssid, sizeof(current_ssid)) != 0) {
        current_ssid[0] = '\0';
    }

    /* parse all entries */
    char* saveptr = NULL;
    char* line = strtok_r(scan_buf, "\n", &saveptr);
    line = strtok_r(NULL, "\n", &saveptr); /* skip header */
    while (line != NULL && ap_count < MAX_SCAN_LINES) {
        ap_entry_t* ap = &aps[ap_count];
        memset(ap, 0, sizeof(*ap));
        if (sscanf(line, "%63[^\t]\t%d\t%d\t%127[^\t]\t%63[^\n]",
                ap->bssid, &ap->freq, &ap->signal, ap->flags, ap->ssid)
            == 5) {
            if (ap->ssid[0] != '\0') {
                ap->saved = 0;
                ap->connected = (current_ssid[0] != '\0' && strcmp(ap->ssid, current_ssid) == 0) ? 1 : 0;
                for (int j = 0; j < known_count; j++) {
                    if (strcmp(ap->ssid, known[j].ssid) == 0) {
                        ap->saved = 1;
                        if (strstr(known[j].flags, "[CURRENT]") != NULL) {
                            ap->connected = 1;
                        }
                        break;
                    }
                }
                ap_count++;
            }
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    /* sort by signal strength descending */
    qsort(aps, ap_count, sizeof(ap_entry_t), ap_cmp);

    snprintf(resp, sizeof(resp), "OK\tSCAN\t%d\n", scan_id);
    send_line(fd, resp);

    MLOG_INFO("SCAN_GET: scan_id=%d, ap_count=%d", scan_id, ap_count);

    /* send sorted entries */
    for (int i = 0; i < ap_count; i++) {
        ap_entry_t* ap = &aps[i];
        MLOG_DBG("scan result: ssid=%s signal=%d secured=%d saved=%d connected=%d",
            ap->ssid, ap->signal, is_protected(ap->flags) ? 1 : 0, ap->saved, ap->connected);
        char out_line[512];
        snprintf(out_line, sizeof(out_line), "AP\t%.63s\t%d\t%d\t%d\t%d\n",
            ap->ssid, ap->signal,
            is_protected(ap->flags) ? 1 : 0, ap->saved, ap->connected);
        send_line(fd, out_line);
    }
    MLOG_INFO("SCAN_GET: scan_id=%d ap_count=%d", scan_id, ap_count);
    send_line(fd, "END\n");
}

static void handle_get_status(int fd)
{
    int enabled = g_enabled ? 1 : 0;
    int connected = 0;
    int rssi_dbm = -1;
    char status[BUF_SIZE];
    char resp[96];

    if (enabled && run_cmd("STATUS", status, sizeof(status)) == 0) {
        connected = is_connected_from_status(status) ? 1 : 0;
        if (connected) {
            rssi_dbm = get_rssi_dbm();
        }
    }

    MLOG_INFO("GET_STATUS: enabled=%d connected=%d rssi=%d", enabled, connected, rssi_dbm);
    snprintf(resp, sizeof(resp), "OK\tSTATUS\t%d\t%d\t%d\n", enabled, connected, rssi_dbm);
    send_line(fd, resp);
}

static void handle_disconnect(int fd)
{
    char out[BUF_SIZE];

    if (run_cmd("DISCONNECT", out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        send_line(fd, "ERR\tDISCONNECT\n");
        return;
    }
    set_connect_status(CONNECT_STATE_IDLE, "", "NONE");
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

static const char* connect_once(const char* ssid, const char* password)
{
    known_network_t known[MAX_NETWORK_LINES];
    int known_count;
    int id;
    char cmd[256];
    char quoted_ssid[WIFI_MAX_SSID_LEN * 2 + 4];
    char quoted_password[WIFI_MAX_PASSWORD_LEN * 2 + 4];
    char out[BUF_SIZE];

    if (ssid == NULL || ssid[0] == '\0') {
        return "EINVAL";
    }

    MLOG_INFO("CONNECT: ssid=%s", ssid);
    known_count = parse_known_networks(known, MAX_NETWORK_LINES);
    id = find_network_id_by_ssid(ssid, known, known_count);

    if (id < 0) {
        if (run_cmd("ADD_NETWORK", out, sizeof(out)) != 0) {
            MLOG_ERR("ADD_NETWORK failed");
            return "ADD_NETWORK";
        }
        id = atoi(out);

        if (quote_wpa_value(ssid, quoted_ssid, sizeof(quoted_ssid)) != 0) {
            return "EINVAL";
        }
        snprintf(cmd, sizeof(cmd), "SET_NETWORK %d ssid %s", id, quoted_ssid);
        if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
            MLOG_ERR("SET_NETWORK ssid failed");
            return "SET_SSID";
        }

        if (password == NULL || password[0] == '\0') {
            snprintf(cmd, sizeof(cmd), "SET_NETWORK %d key_mgmt NONE", id);
        } else {
            if (quote_wpa_value(password, quoted_password, sizeof(quoted_password)) != 0) {
                return "EINVAL";
            }
            snprintf(cmd, sizeof(cmd), "SET_NETWORK %d psk %s", id, quoted_password);
        }
        if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
            MLOG_ERR("SET_NETWORK psk failed");
            return "SET_PSK";
        }
    }

    snprintf(cmd, sizeof(cmd), "SELECT_NETWORK %d", id);
    if (run_cmd(cmd, out, sizeof(out)) != 0 || strstr(out, "OK") == NULL) {
        MLOG_ERR("SELECT_NETWORK failed");
        return "SELECT_NETWORK";
    }

    (void)run_cmd("SAVE_CONFIG", out, sizeof(out));

    {
        int wait_ret = wait_connected(ssid);
        if (wait_ret == CONNECT_WAIT_AUTH_FAILED) {
            MLOG_ERR("CONNECT auth failed: %s", ssid);
            forget_network_id(id);
            return "CONNECT_AUTH_FAILED";
        }
        if (wait_ret != CONNECT_WAIT_OK) {
            MLOG_ERR("CONNECT timeout: %s", ssid);
            return "CONNECT_TIMEOUT";
        }
    }

    MLOG_INFO("CONNECTED: %s", ssid);
    return NULL;
}

static void* connect_worker_thread(void* arg)
{
    connect_task_t* task = (connect_task_t*)arg;
    const char* err = NULL;

    if (task == NULL) {
        finish_connect_worker(CONNECT_STATE_FAILED, "INTERNAL");
        return NULL;
    }

    err = connect_once(task->ssid, task->password);
    if (err == NULL) {
        finish_connect_worker(CONNECT_STATE_CONNECTED, "NONE");
    } else {
        finish_connect_worker(CONNECT_STATE_FAILED, err);
    }

    free(task);
    return NULL;
}

static void handle_connect(int fd, const char* ssid, const char* password)
{
    pthread_t tid;
    connect_task_t* task;

    if (ssid == NULL || ssid[0] == '\0') {
        send_line(fd, "ERR\tEINVAL\n");
        return;
    }
    if (!g_enabled) {
        send_line(fd, "ERR\tWIFI_DISABLED\n");
        return;
    }
    if (try_start_connect_worker(ssid) != 0) {
        send_line(fd, "ERR\tCONNECT_BUSY\n");
        return;
    }

    task = (connect_task_t*)calloc(1, sizeof(*task));
    if (task == NULL) {
        finish_connect_worker(CONNECT_STATE_FAILED, "NOMEM");
        send_line(fd, "ERR\tNOMEM\n");
        return;
    }
    snprintf(task->ssid, sizeof(task->ssid), "%s", ssid);
    if (password != NULL) {
        snprintf(task->password, sizeof(task->password), "%s", password);
    }

    if (pthread_create(&tid, NULL, connect_worker_thread, task) != 0) {
        free(task);
        finish_connect_worker(CONNECT_STATE_FAILED, "THREAD_CREATE");
        send_line(fd, "ERR\tTHREAD_CREATE\n");
        return;
    }
    pthread_detach(tid);
    send_line(fd, "OK\tCONNECTING\n");
}

static void handle_get_connect_result(int fd)
{
    char resp[256];
    int state;
    char error[64];
    char ssid[WIFI_MAX_SSID_LEN];

    pthread_mutex_lock(&g_connect_mutex);
    state = (int)g_connect_state;
    snprintf(error, sizeof(error), "%s", g_connect_error);
    snprintf(ssid, sizeof(ssid), "%s", g_connect_ssid[0] != '\0' ? g_connect_ssid : "-");
    pthread_mutex_unlock(&g_connect_mutex);

    snprintf(resp, sizeof(resp), "OK\tCONNECT_RESULT\t%d\t%s\t%s\n", state, error, ssid);
    send_line(fd, resp);
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
        } else if (strcmp(cmd, "GET_CONNECT_RESULT") == 0) {
            handle_get_connect_result(cfd);
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

    MLOG_OPEN();
    MLOG_INFO("wifi-daemon starting...");

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    signal(SIGPIPE, SIG_IGN);

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
