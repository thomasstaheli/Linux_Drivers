#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define NB_DATA 128

int main(void)
{
	int fd;
	int i;
	int nb_to_write;
	int nb_read;
	int success;
	uint8_t datas[NB_DATA];
	uint8_t datas_read[NB_DATA];

	fd = open("/dev/parrot", O_RDWR);
	if (fd < 0) {
		perror("parrot_test");
	}

	// Initialize the datas array
	printf("Written data are:\n");
	for (i = 0; i < NB_DATA; i++) {
		datas[i] = i;

		printf("0x%02x ", datas[i]);

		if ((i & 0xf) == 0xf) {
			printf("\n");
		}
	}
	printf("\n");

	// Write the array in the driver
	for (i = 0; i < NB_DATA;) {
		// Randomly choose the number of data actually written to test the driver
		// with differents size.
		nb_to_write = (rand() % 32) + 1;

		// Don't overflow
		if (i + nb_to_write > NB_DATA) {
			nb_to_write = NB_DATA - i;
		}

		if (write(fd, &datas[i], nb_to_write) != nb_to_write) {
			printf("Not all data have been written\n");
			return EXIT_FAILURE;
		}

		i += nb_to_write;
	}

	lseek(fd, 0, SEEK_SET);

	// Read the data back
	nb_read = 0;
	do {
		i = read(fd, &datas_read[nb_read], 10);
		if (i < 0) {
			printf("Error while reading datas\n");
		}

		nb_read += i;
	} while (i);

	if (nb_read != NB_DATA) {
		printf("Not enough or too much data read (expected %d got %d)\n",
		       NB_DATA, nb_read);
		return EXIT_FAILURE;
	}

	// Compare the array
	printf("Read data are:\n");
	success = 1;
	for (i = 0; i < nb_read; i++) {
		success &= datas[i] == datas_read[i];

		printf("0x%02x ", datas_read[i]);

		if ((i & 0xf) == 0xf) {
			printf("\n");
		}
	}

	printf("\n");
	if (success) {
		printf("All data were correct\n");
	} else {
		printf("Some data are incorrect\n");
	}

	return EXIT_SUCCESS;
}
