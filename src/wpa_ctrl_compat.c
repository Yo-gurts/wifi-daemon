#include <stddef.h>
#include "third_party/wpa_ctrl.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct wpa_ctrl {
    int s;
    struct sockaddr_un local;
    struct sockaddr_un dest;
};

static int next_ctrl_id = 0;

struct wpa_ctrl* wpa_ctrl_open2(const char *ctrl_path, const char *cli_path)
{
    struct wpa_ctrl *ctrl;
    size_t len;

    if (ctrl_path == NULL) {
        return NULL;
    }

    ctrl = (struct wpa_ctrl*)calloc(1, sizeof(*ctrl));
    if (ctrl == NULL) {
        return NULL;
    }

    ctrl->s = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl->s < 0) {
        free(ctrl);
        return NULL;
    }

    memset(&ctrl->local, 0, sizeof(ctrl->local));
    ctrl->local.sun_family = AF_UNIX;
    if (cli_path && cli_path[0]) {
        snprintf(ctrl->local.sun_path, sizeof(ctrl->local.sun_path), "%s", cli_path);
    } else {
        snprintf(ctrl->local.sun_path, sizeof(ctrl->local.sun_path), "/tmp/wpa_ctrl_%d_%d", (int)getpid(), next_ctrl_id++);
    }
    unlink(ctrl->local.sun_path);

    len = sizeof(ctrl->local);
    if (bind(ctrl->s, (struct sockaddr*)&ctrl->local, len) < 0) {
        close(ctrl->s);
        free(ctrl);
        return NULL;
    }

    memset(&ctrl->dest, 0, sizeof(ctrl->dest));
    ctrl->dest.sun_family = AF_UNIX;
    snprintf(ctrl->dest.sun_path, sizeof(ctrl->dest.sun_path), "%s", ctrl_path);

    len = sizeof(ctrl->dest);
    if (connect(ctrl->s, (struct sockaddr*)&ctrl->dest, len) < 0) {
        unlink(ctrl->local.sun_path);
        close(ctrl->s);
        free(ctrl);
        return NULL;
    }

    return ctrl;
}

struct wpa_ctrl* wpa_ctrl_open(const char *ctrl_path)
{
    return wpa_ctrl_open2(ctrl_path, NULL);
}

void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
    if (ctrl == NULL) {
        return;
    }
    unlink(ctrl->local.sun_path);
    close(ctrl->s);
    free(ctrl);
}

int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len,
                     char *reply, size_t *reply_len,
                     void (*msg_cb)(char *msg, size_t len))
{
    struct pollfd pfd;
    int ret;

    (void)msg_cb;

    if (ctrl == NULL || cmd == NULL || reply == NULL || reply_len == NULL) {
        return -1;
    }

    if (send(ctrl->s, cmd, cmd_len, 0) < 0) {
        return -1;
    }

    pfd.fd = ctrl->s;
    pfd.events = POLLIN;
    ret = poll(&pfd, 1, 3000);
    if (ret <= 0) {
        return -2;
    }

    ret = recv(ctrl->s, reply, *reply_len, 0);
    if (ret < 0) {
        return -1;
    }
    *reply_len = (size_t)ret;
    if ((size_t)ret < *reply_len) {
        reply[ret] = '\0';
    }
    return 0;
}

int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
    char buf[32];
    size_t len = sizeof(buf) - 1;

    if (wpa_ctrl_request(ctrl, "ATTACH", 6, buf, &len, NULL) != 0) {
        return -1;
    }
    buf[len] = '\0';
    return strstr(buf, "OK") ? 0 : -1;
}

int wpa_ctrl_detach(struct wpa_ctrl *ctrl)
{
    char buf[32];
    size_t len = sizeof(buf) - 1;

    if (wpa_ctrl_request(ctrl, "DETACH", 6, buf, &len, NULL) != 0) {
        return -1;
    }
    buf[len] = '\0';
    return strstr(buf, "OK") ? 0 : -1;
}

int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
    int ret;

    if (ctrl == NULL || reply == NULL || reply_len == NULL) {
        return -1;
    }

    ret = recv(ctrl->s, reply, *reply_len, 0);
    if (ret < 0) {
        return -1;
    }
    *reply_len = (size_t)ret;
    return 0;
}

int wpa_ctrl_pending(struct wpa_ctrl *ctrl)
{
    struct pollfd pfd;

    if (ctrl == NULL) {
        return -1;
    }

    pfd.fd = ctrl->s;
    pfd.events = POLLIN;
    return poll(&pfd, 1, 0) > 0;
}

int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl)
{
    if (ctrl == NULL) {
        return -1;
    }
    return ctrl->s;
}

void wpa_ctrl_cleanup(void)
{
}

char* wpa_ctrl_get_remote_ifname(struct wpa_ctrl *ctrl)
{
    (void)ctrl;
    return NULL;
}
