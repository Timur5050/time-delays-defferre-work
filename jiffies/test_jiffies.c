#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main()
{
    int fd = open("/dev/simplechartest", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    char buf[1024];

    write(fd, "interval=2000", strlen("interval=2000"));
    write(fd, "test data", strlen("test data"));
    lseek(fd, 0, SEEK_SET);
    sleep(1);  // ще не пройшло 2 секунди
    int ret = read(fd, buf, 1024);
    if (ret < 0)
        perror("read after 1s");
    else {
        buf[ret] = '\0';
        printf("after 1s: %s\n", buf);
    }
    lseek(fd, 0, SEEK_SET);
    sleep(2);  // тепер пройшло
    ret = read(fd, buf, 1024);
    if (ret < 0)
        perror("read after 2s");
    else {
        buf[ret] = '\0';
        printf("after 2s: %s\n", buf);
    }

    write(fd, "reset", strlen("reset"));
    lseek(fd, 0, SEEK_SET);
    ret = read(fd, buf, 1024);
    if (ret < 0)
        perror("read after reset");
    else {
        buf[ret] = '\0';
        printf("after reset: %s\n", buf);
    }

    close(fd);
    return 0;
}
