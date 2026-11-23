#include <fcntl.h>
#include <unistd.h>

int main(void)
{
    static const char msg[] = "SYSTEMD_OK\n";
    int fd;

    fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        ssize_t ignored = write(fd, msg, sizeof(msg) - 1);
        (void)ignored;
        close(fd);
    }
    return 0;
}
