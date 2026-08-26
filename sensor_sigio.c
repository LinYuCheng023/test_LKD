#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>

struct sampled_data {
    uint32_t seq;
    uint32_t value;
    uint64_t timestamp_ns;
};

static volatile sig_atomic_t sigio_seen;
static volatile sig_atomic_t stop;

static void on_sigio(int sig)
{
    (void)sig;
    sigio_seen = 1;
}

static void on_sigint(int sig)
{
    (void)sig;
    stop = 1;
}

int main(void)
{
    int fd;
    int flags;
    struct sigaction sa = {0};
    struct sampled_data data;
    ssize_t r;
    sigset_t block_set;
    sigset_t old_set;
    sigset_t wait_set;

    sigemptyset(&block_set);
    sigaddset(&block_set, SIGIO);
    sigaddset(&block_set, SIGINT);

    if (sigprocmask(SIG_BLOCK, &block_set, &old_set) < 0) {
        perror("sigprocmask");
        return 1;
    }

    sa.sa_handler = on_sigio;
    sigemptyset(&sa.sa_mask);
    if ( sigaction(SIGIO, &sa, NULL) < 0) {
        perror("sigaction SIGIO");
        return 1;
    }

    sa.sa_handler = on_sigint;
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        perror("sigaction SIGINT");
        return 1;
    }

    fd = open("/dev/sensor_sample", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (fcntl(fd, F_SETOWN, getpid()) < 0) {
        perror("fcntl F_SETOWN");
        close(fd);
        return 1;
    }

    flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        perror("fcntl F_GETFL");
        close(fd);
        return 1;
    }

    if (fcntl(fd, F_SETFL, flags | O_ASYNC) < 0) {
        perror("F_SETFL");
        close(fd);
        return 1;
    }

    printf("等待 SIGIO，按 Ctrl-C 結束...\n");

    sigemptyset(&wait_set);

    while (!stop) {
        while ((r = read(fd, &data, sizeof(data))) == sizeof(data)) {
            printf("SIGIO: seq=%u, value=0x%x\n", data.seq, data.value);
        }

        if (r < 0 && errno == ENODEV) {
            stop = 1;
            break;
        }
            
        if (!sigio_seen && !stop)
            sigsuspend(&wait_set);
        
        if (stop)
            break;

        sigio_seen = 0;
  
    }

    close(fd);
    return 0;
}
