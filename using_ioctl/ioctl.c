#include <stdio.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

int main(int argc, char *argv[])
{
	int fd = -1;
	int rc = 0;

	if (argc != 4) {
		fprintf(stdout,
			"Usage: %s filename cmd param\n"
			"  filename: string\n  cmd: unsigned int\n  param: unsigned int\n",
			argv[0]);
		rc = -1;
		goto close;
	}

	if ((fd = open(argv[1], O_RDONLY)) < 0) {
		fprintf(stderr, "Error opening %s\n", argv[1]);
		rc = -2;
		goto close;
	}

	errno = 0;
	if (ioctl(fd, atoi(argv[2]), atoi(argv[3])) < 0) {
		perror("ioctl:");
		rc = -3;
		goto close;
	}

close:
	close(fd);
	return rc;
}
