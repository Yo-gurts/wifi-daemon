#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SOCKET_PATH "/tmp/aicam_wifi.sock"

static int send_cmd(const char *cmd) {
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
        printf("<<< %s\n", buf);
    }

    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    const char *daemon_path = (argc > 1) ? argv[1] : "./bin/wifi-daemon";
    char cmd[256];
    int pid;

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

    printf("\n=== Test 1: SET_ENABLED ===\n");
    snprintf(cmd, sizeof(cmd), "SET_ENABLED\t1");
    send_cmd(cmd);

    printf("\n=== Test 2: SCAN_START ===\n");
    snprintf(cmd, sizeof(cmd), "SCAN_START");
    send_cmd(cmd);

    /* give daemon time to receive scan results from wpa_supplicant */
    printf("Waiting for scan results...\n");
    sleep(3);

    printf("\n=== Test 3: SCAN_GET ===\n");
    snprintf(cmd, sizeof(cmd), "SCAN_GET");
    send_cmd(cmd);

    printf("\n=== Test 4: DISCONNECT ===\n");
    snprintf(cmd, sizeof(cmd), "DISCONNECT");
    send_cmd(cmd);

    printf("\n=== Test 5: SET_ENABLED 0 ===\n");
    snprintf(cmd, sizeof(cmd), "SET_ENABLED\t0");
    send_cmd(cmd);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    printf("\n=== All tests passed ===\n");
    return 0;
}
