/*
* Butchered from the dnbd3-fuse by C.K.
**/

#include "connection.h"
#include "helper.h"
#include "../shared/protocol.h"
#include "../shared/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <getopt.h>
#include <pthread.h>

#define debugf(...) do { logadd( LOG_DEBUG1, __VA_ARGS__ ); } while (0)


/* Debug/Benchmark variables */
static bool useDebug = false;


static void printUsage(char *argv0, int exitCode)
{
	printf( "Usage: %s [--debug] --host <serverAddress(es)> --image <imageName> [--rid revision]\n", argv0 );
	printf( "Or:    %s [-d] -h <serverAddress(es)> -i <imageName> [-r revision]\n", argv0 );
	printf( "   -h --host       List of space separated hosts to use\n" );
	printf( "   -i --image      Remote image name to request\n" );
	printf( "   -r --rid        Revision to use (omit or pass 0 for latest)\n" );
	printf( "   -n --runs       Number of connection attempts per thread\n" );
	printf( "   -t --threads    number of threads\n" );
	printf( "   -l --log        Write log to given location\n" );
	printf( "   -d --debug      Don't fork and print debug output (fuse > stderr, dnbd3 > stdout)\n" );
	// // fuse_main( 2, arg, &dnbd3_fuse_no_operations, NULL );
	exit( exitCode );
}

static const char *optString = "h:i:n:t:HvVd";
static const struct option longOpts[] = {
        { "host", required_argument, NULL, 'h' },
        { "image", required_argument, NULL, 'i' },
        { "nruns", optional_argument, NULL, 'n' },
        { "threads", optional_argument, NULL, 't' },
        { "help", optional_argument, NULL, 'H' },
        { "version", no_argument, NULL, 'v' },
        { 0, 0, 0, 0 }
};


static void printBenchCounters(BenchCounters* c) {
	printf ("Attempts:\t%d\n", c->attempts);
	printf ("Success :\t%d\n", c->success);
	printf ("Fails   :\t%d\n", c->fails);
}


void* runBenchThread(void* t) {
	BenchThreadData* data = t;
	connection_init_n_times(
			data->server_address,
			data->server_address,
			0,
			data->runs,
			data->counter,
			data->closeSockets);
	printf("Thread #%d finished\n", data->threadNumber);
	return NULL;
}

int main(int argc, char *argv[])
{
	char *server_address = NULL;
	char *image_Name = NULL;
	int opt, lidx;

	bool closeSockets = false;
	int n_runs = 100;
	int n_threads = 1;

	if ( argc <= 1 || strcmp( argv[1], "--help" ) == 0 || strcmp( argv[1], "--usage" ) == 0 ) {
		printUsage( argv[0], 0 );
	}

	while ( ( opt = getopt_long( argc, argv, optString, longOpts, &lidx ) ) != -1 ) {
		switch ( opt ) {
		case 'h':
			server_address = optarg;
			break;
		case 'i':
			image_Name = optarg;
			break;
		case 'n':
			n_runs = atoi(optarg);
			break;
		case 't':
			n_threads = atoi(optarg);
			break;
		case 'c':
			closeSockets = true;
			break;
		case 'H':
			printUsage( argv[0], 0 );
			break;
		case 'd':
			useDebug = true;
			break;
		default:
			printUsage( argv[0], EXIT_FAILURE );
		}
	}

	printf("Welcome to dnbd3 benchmark tool\n");

	/* all counters */
	BenchCounters 		counters[n_threads];
	BenchThreadData 	threadData[n_threads];
	pthread_t 			threads[n_threads];

	/* create all threads */
	for (int i = 0; i < n_threads; i++) {
		BenchCounters tmp1 = {0,0,0};
		counters[i] = tmp1;
		BenchThreadData tmp2 = {
			&(counters[i]),
			server_address,
			image_Name,
			n_runs,
			i,
			closeSockets};
		threadData[i] = tmp2;
		pthread_create(&(threads[i]), NULL, runBenchThread, &(threadData[i]));
	}


	/* join all threads*/
	for (int i = 0; i < n_threads; ++i) {
		pthread_join(threads[i], NULL);
	}

	/* print out all counters & sum up */
	BenchCounters total = {0,0,0};
	for (int i = 0; i < n_threads; ++i) {
		printf("#### Thread %d\n", i);
		printBenchCounters(&counters[i]);
		total.attempts += counters[i].attempts;
		total.success += counters[i].success;
		total.fails += counters[i].fails;
	}
	/* print out summary */
	printf("\n\n#### SUMMARY\n");
	printBenchCounters(&total);
	printf("\n-- End of program");
}
