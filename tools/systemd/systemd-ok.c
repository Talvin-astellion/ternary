#include <fcntl.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

static void console_message(const char *msg)
{
    int fd = open("/dev/console", O_WRONLY);

    if (fd >= 0) {
        ssize_t ignored = write(fd, msg, strlen(msg));
        (void)ignored;
        close(fd);
    }
}

int main(void)
{
    static const char ok[] = "SYSTEMD_OK\n";
    static const char failed[] = "SYSTEMD_POWEROFF_FAILED\n";

    console_message(ok);
    sync();
    if (reboot(RB_POWER_OFF) != 0) {
        console_message(failed);
        return 1;
    }
    return 0;
}
