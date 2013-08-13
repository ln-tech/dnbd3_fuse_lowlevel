#include "integrity.h"

#include "locks.h"
#include "image.h"
#include "globals.h"
#include "memlog.h"
#include "helper.h"

#include <pthread.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>

#define CHECK_QUEUE_SIZE 100

typedef struct
{
	dnbd3_image_t * volatile image;
	int volatile block;
} queue_entry;

static pthread_t thread;
static queue_entry checkQueue[CHECK_QUEUE_SIZE];
static pthread_mutex_t integrityQueueLock;
static pthread_cond_t queueSignal;
static int queueLen = -1;
static int bRunning = FALSE;

static void* integrity_main(void *data);

/**
 * Initialize the integrity check thread
 */
void integrity_init()
{
	assert( queueLen == -1 );
	pthread_mutex_init( &integrityQueueLock, NULL );
	pthread_cond_init( &queueSignal, NULL );
	bRunning = TRUE;
	if ( 0 != pthread_create( &thread, NULL, &integrity_main, (void *)NULL ) ) {
		bRunning = FALSE;
		memlogf( "[WARNING] Could not start integrity check thread. Corrupted images will not be detected." );
		return;
	}
	queueLen = 0;
}

void integrity_shutdown()
{
	assert( queueLen != -1 );
	printf( "[DEBUG] Shutting down integrity checker...\n" );
	pthread_mutex_lock( &integrityQueueLock );
	pthread_cond_signal( &queueSignal );
	pthread_mutex_unlock( &integrityQueueLock );
	pthread_join( thread, NULL );
	while ( bRunning )
		usleep( 10000 );
	pthread_mutex_destroy( &integrityQueueLock );
	pthread_cond_destroy( &queueSignal );
	printf( "[DEBUG] Integrity checker exited normally.\n" );
}

/**
 * Schedule an integrity check on the given image for the given hash block.
 * It is not checked whether the block is completely cached locally, so
 * make sure it is before calling, otherwise it will result in falsely
 * detected corruption.
 */
void integrity_check(dnbd3_image_t *image, int block)
{
	int i, freeSlot = -1;
	pthread_mutex_lock( &integrityQueueLock );
	for (i = 0; i < queueLen; ++i) {
		if ( freeSlot == -1 && checkQueue[i].image == NULL ) {
			freeSlot = i;
		} else if ( checkQueue[i].image == image && checkQueue[i].block == block ) {
			pthread_mutex_unlock( &integrityQueueLock );
			return;
		}
	}
	if ( freeSlot == -1 ) {
		if ( queueLen >= CHECK_QUEUE_SIZE ) {
			pthread_mutex_unlock( &integrityQueueLock );
			printf( "[DEBUG] Check queue full, discarding check request...\n" );
			return;
		}
		freeSlot = queueLen++;
	}
	checkQueue[freeSlot].image = image;
	checkQueue[freeSlot].block = block;
	pthread_cond_signal( &queueSignal );
	pthread_mutex_unlock( &integrityQueueLock );
}

static void* integrity_main(void *data)
{
	int i;
	uint8_t *buffer = NULL;
	size_t bufferSize = 0;
	setThreadName( "image-check" );
	pthread_mutex_lock( &integrityQueueLock );
	while ( !_shutdown ) {
		for (i = queueLen - 1; i >= 0; --i) {
			if ( checkQueue[i].image == NULL ) continue;
			dnbd3_image_t * const image = image_lock( checkQueue[i].image );
			checkQueue[i].image = NULL;
			if ( image == NULL ) continue;
			// We have the image. Call image_release() some time
			if ( i + 1 == queueLen ) queueLen--;
			spin_lock( &image->lock );
			if ( image->crc32 != NULL && image->filesize != 0 ) {
				int const blocks[2] = { checkQueue[i].block, -1 };
				pthread_mutex_unlock( &integrityQueueLock );
				const uint64_t fileSize = image->filesize;
				const size_t required = IMGSIZE_TO_HASHBLOCKS(image->filesize) * sizeof(uint32_t);
				if ( required > bufferSize ) {
					bufferSize = required;
					if ( buffer != NULL ) free( buffer );
					buffer = malloc( bufferSize );
				}
				memcpy( buffer, image->crc32, required );
				spin_unlock( &image->lock );
				int fd = open( image->path, O_RDONLY );
				if ( fd >= 0 ) {
					if ( image_checkBlocksCrc32( fd, (uint32_t*)buffer, blocks, fileSize ) ) {
						printf( "[DEBUG] CRC check of block %d for %s succeeded :-)\n", blocks[0], image->lower_name );
					} else {
						memlogf( "[WARNING] Hash check for block %d of %s failed!", blocks[0], image->lower_name );
						image_updateCachemap( image, blocks[0] * HASH_BLOCK_SIZE, (blocks[0] + 1) * HASH_BLOCK_SIZE, FALSE );
					}
					close( fd );
				}
				pthread_mutex_lock( &integrityQueueLock );
			} else {
				spin_unlock( &image->lock );
			}
			// Release :-)
			image_release( image );
		}
		if ( queueLen == 0 ) {
			pthread_cond_wait( &queueSignal, &integrityQueueLock );
		}
	}
	pthread_mutex_unlock( &integrityQueueLock );
	if ( buffer != NULL ) free( buffer );
	bRunning = FALSE;
	return NULL ;
}