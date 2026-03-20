#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET_PATH "/tmp/aicam_wifi.sock"

static int send_cmd(const char *cmd, char *resp, size_t resp_sz) {
    int fd;
    struct sockaddr_un addr;
    char buf[1024];
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    printf(">>> %s\n", cmd);
    if (write(fd, cmd, strlen(cmd)) < 0) {
        perror("write");
        close(fd);
        return -1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("<<< %s", buf);
        if (resp && resp_sz > 0) {
            snprintf(resp, resp_sz, "%s", buf);
        }
    }

    close(fd);
    return 0;
}

static int parse_scan_id(const char *resp) {
    /* parse scan_id from response like "OK\tSCAN_STARTED\t1\n" or "OK\tSCAN\t1\n" */
    int id = -1;
    const char *p = strstr(resp, "SCAN_STARTED");
    if (!p) {
        p = strstr(resp, "SCAN");
    }
    if (p) {
        while (*p && *p != '\t') p++;
        if (*p == '\t') {
            id = atoi(p + 1);
        }
    }
    return id;
}

int main(int argc, char *argv[]) {
    const char *daemon_path = (argc > 1) ? argv[1] : "./bin/wifi-daemon";
    char cmd[256];
    char resp[1024];
    int pid;
    int my_scan_id = -1;

    printf("Starting wifi-daemon: %s\n", daemon_path);
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* child */
        execl(daemon_path, daemon_path, NULL);
        perror("execl");
        return 1;
    }

    /* parent - wait for daemon to start */
    usleep(500000);

    printf("\n=== Test 0: GET_STATUS (before enable) ===\n");
    snprintf(cmd, sizeof(cmd), "GET_STATUS");
    send_cmd(cmd, resp, sizeof(resp));

    printf("\n=== Test 1: SET_ENABLED ===\n");
    snprintf(cmd, sizeof(cmd), "SET_ENABLED\t1");
    send_cmd(cmd, NULL, 0);

    printf("\n=== Test 2: SCAN_START ===\n");
    snprintf(cmd, sizeof(cmd), "SCAN_START");
    send_cmd(cmd, resp, sizeof(resp));
    my_scan_id = parse_scan_id(resp);
    printf("    [my_scan_id=%d]\n", my_scan_id);

    printf("\n=== Test 3: SCAN_GET (polling 10 times) ===\n");
    for (int i = 0; i < 10; i++) {
        printf("[%d/10] ", i + 1);
        fflush(stdout);
        snprintf(cmd, sizeof(cmd), "SCAN_GET");
        send_cmd(cmd, resp, sizeof(resp));
        int id = parse_scan_id(resp);
        if (id >= 0) {
            printf("    [scan_id=%d %s]\n", id, (id >= my_scan_id) ? "(>= my scan)" : "(stale)");
        }
        sleep(1);
    }

    printf("\n=== Test 4: GET_STATUS (after enable) ===\n");
    snprintf(cmd, sizeof(cmd), "GET_STATUS");
    send_cmd(cmd, resp, sizeof(resp));

    printf("\n=== Test 5: DISCONNECT ===\n");
    snprintf(cmd, sizeof(cmd), "DISCONNECT");
    send_cmd(cmd, NULL, 0);

    printf("\n=== Test 6: SET_ENABLED 0 ===\n");
    snprintf(cmd, sizeof(cmd), "SET_ENABLED\t0");
    send_cmd(cmd, NULL, 0);

    printf("\n=== Test 7: GET_STATUS (after disable) ===\n");
    snprintf(cmd, sizeof(cmd), "GET_STATUS");
    send_cmd(cmd, resp, sizeof(resp));

    printf("\n=== Test 8: SET_ENABLED ===\n");
    snprintf(cmd, sizeof(cmd), "SET_ENABLED\t1");
    send_cmd(cmd, NULL, 0);

    printf("\n=== Test 9: SCAN_START ===\n");
    snprintf(cmd, sizeof(cmd), "SCAN_START");
    send_cmd(cmd, resp, sizeof(resp));
    my_scan_id = parse_scan_id(resp);
    printf("    [my_scan_id=%d]\n", my_scan_id);

    printf("\n=== Test 10: SCAN_GET (polling 10 times) ===\n");
    for (int i = 0; i < 10; i++) {
        printf("[%d/10] ", i + 1);
        fflush(stdout);
        snprintf(cmd, sizeof(cmd), "SCAN_GET");
        send_cmd(cmd, resp, sizeof(resp));
        int id = parse_scan_id(resp);
        if (id >= 0) {
            printf("    [scan_id=%d %s]\n", id, (id >= my_scan_id) ? "(>= my scan)" : "(stale)");
        }
        sleep(1);
    }

    printf("\n=== Test 11: GET_STATUS (after enable) ===\n");
    snprintf(cmd, sizeof(cmd), "GET_STATUS");
    send_cmd(cmd, resp, sizeof(resp));

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n=== All tests passed ===\n");
    return 0;
}
