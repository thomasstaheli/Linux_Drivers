// Thomas Stäheli
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdint.h>

#define DEVICE_FILE "/dev/accumulate"

// Commandes ioctl (doivent correspondre au driver)
#define ACCUMULATE_CMD_RESET 11008
#define ACCUMULATE_CMD_CHANGE_OP 1074014977
#define OP_ADD 0
#define OP_MULTIPLY 1

int main() {
    int fd = open(DEVICE_FILE, O_RDWR);
    if (fd < 0) {
        perror("Failed to open device");
        return EXIT_FAILURE;
    }

    // Test addition
    ioctl(fd, ACCUMULATE_CMD_CHANGE_OP, OP_ADD);
    ioctl(fd, ACCUMULATE_CMD_RESET, 0);
    
    uint64_t values[] = {5, 3, 10};
    for (int i = 0; i < 3; i++) {
        write(fd, &values[i], sizeof(values[i]));
    }
    
    uint64_t result;
    read(fd, &result, sizeof(result));
    printf("Addition result: %lu (expected 18)\n", result);

    // Test multiplication
    ioctl(fd, ACCUMULATE_CMD_RESET, 0); 
    uint64_t default_value = 1;
    write(fd, &default_value, sizeof(default_value));
    ioctl(fd, ACCUMULATE_CMD_CHANGE_OP, OP_MULTIPLY);
    
    for (int i = 0; i < 3; i++) {
        write(fd, &values[i], sizeof(values[i]));
    }
    
    read(fd, &result, sizeof(result));
    printf("Multiplication result: %lu (expected 150)\n", result);

    close(fd);
    return EXIT_SUCCESS;
}