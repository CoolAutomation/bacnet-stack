#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <string.h>

#include <time.h>
#include <stdarg.h>

#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_PIN 0

int fd;
struct gpiohandle_request req;

int init_gpio() {
    fd = open(GPIO_CHIP, O_RDWR);
    if (fd < 0) {
        perror("Failed to open GPIO chip");
        return -1;
    }

    req.lineoffsets[0] = GPIO_PIN;
    req.flags = GPIOHANDLE_REQUEST_OUTPUT;
    req.lines = 1;
    strncpy(req.consumer_label, "gpio_test", sizeof(req.consumer_label));

    // Request the GPIO line
    if (ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req) < 0) {
        perror("Failed to get line handle");
        close(fd);
        return -1;
    }

    return 0;
}

void set_gpio(int value) {
    struct gpiohandle_data data;
    data.values[0] = value;

    return ioctl(req.fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data) < 0;
}

void fprintf_curtime(FILE *stream, const char *format, ...) {
    // Get the current time
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Convert to a time structure
    struct tm *tm_info = localtime(&ts.tv_sec);

    // Prepare a buffer for the timestamp
    char timestamp[30];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

    // Print the timestamp with millisecond precision
    fprintf(stream, "TIMING %s.%03ld ", timestamp, ts.tv_nsec / 1000000);

    // Handle variable arguments for fprintf
    va_list args;
    va_start(args, format);
    vfprintf(stream, format, args);
    va_end(args);
}
