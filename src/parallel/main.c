// main.c
//
// "THE BEER-WARE LICENSE" (Revision 42):
// <filipeutzig@gmail.com> wrote this file. As long as you retain this
// notice you can do whatever you want with this stuff. If we meet some
// day, and you think this stuff is worth it, you can buy me a beer in
// return.
//
// Initial version by Filipe Utzig <filipeutzig@gmail.com> on 3/26/15.
//
// The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
// "SHOULD", "SHOULD NOT", "RECOMMENDED", "MAY", and "OPTIONAL" in
// this document are to be interpreted as described in RFC 2119.
//
// Yet another sort app using Insertion Sort algorithm
// (Parallel MPI implementation)
//

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <utils.h>
#include <insertion_sort.h>
#include <mpi.h>

#define MAX_ELEM                                    (100000)
#define MPI_TAG                                     (0)
#define MPI_TERMINATE                               (-1)
#define FILENAME                                    "sorted_vector.txt"

void
print_usage (void)
{
	printf("Usage: %s FILE SIZE\n", PROG_NAME);
	printf("\tFILE File containing vector data\n");
	printf("\tSIZE Size of vector (Maximum size: %d)\n", MAX_ELEM);
}

/**
 * @brief First stage of pipeline architecture of MPI.
 *
 * @param file filename of messy vector
 * @param jobs number of pipeline stages
 * @param size size of vector
 */
static void
do_first_stage (const char *file, int jobs, size_t size)
{
	int i;
	int ret;
	int val;
	int *readv;
	int *sortv;
	size_t slice;
	struct timeval begin;
	struct timeval end;
	FILE *fd;
	MPI_Status status;

	fd = fopen(file, "r");
	if (!fd) {
		print_errno("fopen() failed!");
		exit(1);
	}

	slice = size / jobs;
	readv = calloc(size, sizeof(*readv));
	sortv = calloc(slice, sizeof(*sortv));
	for (i = 0; (size_t) i < size; i++) {
		ret = fscanf(fd, "%d", &readv[i]);
		if (ret < 0) {
			print_errno("fscanf() failed!");
			exit(ret);
		}
	}
	fclose(fd);

	gettimeofday(&begin, NULL);

	// To fill the sorted vector, just do a sequential insertion sort
	// algorithm for the firsts `SLICE` elements
	insertion_sortv(readv, sortv, slice);

	// Now we need to analize the values and forward to pipeline
	for (i = slice; (size_t) i < size; i++) {
		val = readv[i];
		print_debug("i=%d val=%d", i, val);
		insertion_sort(sortv, slice, &val);
		print_debug("ret val=%d", val);
		MPI_Send(&val, 1, MPI_INT, 1, MPI_TAG, MPI_COMM_WORLD);
	}

	// Wait for the last pipeline stage done their job
	MPI_Recv(&val, 1, MPI_INT, jobs - 1, MPI_TAG, MPI_COMM_WORLD, &status);

	gettimeofday(&end, NULL);

	print_time(begin, end);
//	SAVE_RESULTS(RESULTS_WRITE, sortv, slice);
	for (i = 0; i < slice; i++) {
		print_debug("stage=0 buf[%d]=%d", i, sortv[i]);
	}
	sleep(1);

	val = MPI_TERMINATE;
	MPI_Send(&val, 1, MPI_INT, 1, MPI_TAG, MPI_COMM_WORLD);

	printf("The result can be found at file '%s'\n", FILENAME);

	free(readv);
	free(sortv);
}

/**
 * @brief Another stages of pipeline architecture of MPI.
 *
 * @param stage pipeline stage identifier
 * @param jobs number of pipeline stages
 * @param size size of vector
 */
static void
do_pipeline (int stage, int jobs, size_t size)
{
	int val;
	int prev;
	int next;
	int *buf;
	size_t recv;
	size_t slice;
	size_t elements;
	MPI_Status status;

	if (stage != jobs - 1) {
		slice = size / jobs;
		elements = size - slice * stage;
		next = stage + 1;
	} else {
		// Last stage of pipeline takes the rest of elements
		slice = (size / jobs) + (size % jobs);
		elements = slice;
		next = 0;
	}

	prev = stage - 1;
	print_debug("stage=%d slice=%zu elements=%zu next=%d prev=%d", stage, slice, elements, next, prev);

	buf = calloc(slice, sizeof(*buf));

	recv = 0;
	while (recv < elements) {
		MPI_Recv(&val, 1, MPI_INT, prev, MPI_TAG, MPI_COMM_WORLD,
		    &status);
		print_debug("recv=%d val=%d", recv, val);
		if (recv < slice) {
			// While the vector isn't filled, just do a
			// regular insertion sort
			insertion_sort(buf, recv, &val);
			print_debug("buf[%d]=%d val=%d", recv, buf[recv], val);
		} else {
			// Now we need to forward to pipeline
			insertion_sort(buf, slice, &val);
			print_debug("buf[%d]=%d val=%d", slice, buf[slice], val);
			MPI_Send(&val, 1, MPI_INT, next, MPI_TAG,
			    MPI_COMM_WORLD);
		}
		recv++;
	}
	if (!next) {
		val = MPI_TERMINATE;
		MPI_Send(&val, 1, MPI_INT, next, MPI_TAG, MPI_COMM_WORLD);
	}
	MPI_Recv(&val, 1, MPI_INT, prev, MPI_TAG, MPI_COMM_WORLD, &status);

//	SAVE_RESULTS(RESULTS_APPEND, buf, slice);
	int i;
	for (i = 0; i < slice; i++) {
		print_debug("stage=%d buf[%d]=%d", stage, i, buf[i]);
	}
	sleep(1);
	if (next) {
		val = MPI_TERMINATE;
		MPI_Send(&val, 1, MPI_INT, next, MPI_TAG, MPI_COMM_WORLD);
	}

	free(buf);
}

int
main (int argc, const char **argv)
{
	int id;
	int jobs;
	size_t size;

	prgname = argv[0];

	if (argc != 3) {
		print_usage();
		exit(1);
	}

	size = (size_t) atoi(argv[2]);
	if (size < 1 || size > MAX_ELEM) {
		print_error("Invalid number of elements: %zu!", size);
		exit(1);
	}

	MPI_Init(&argc, (char ***) &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &id);
	MPI_Comm_size(MPI_COMM_WORLD, &jobs);

	if (!id) {
		do_first_stage(argv[1], jobs, size);
	} else {
		do_pipeline(id, jobs, size);
	}

	MPI_Finalize();

	return (0);
}
