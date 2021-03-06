#include "image.h"
#include "helper.h"
#include "fileutil.h"
#include "uplink.h"
#include "locks.h"
#include "integrity.h"
#include "altservers.h"
#include "../shared/protocol.h"
#include "../shared/timing.h"
#include "../shared/crc32.h"

#include <assert.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>
#include <glob.h>
#include <jansson.h>

#define PATHLEN (2000)
#define NONWORKING_RECHECK_INTERVAL_SECONDS (60)

// ##########################################

static dnbd3_image_t *_images[SERVER_MAX_IMAGES];
static int _num_images = 0;

static pthread_mutex_t imageListLock;
static pthread_mutex_t remoteCloneLock;
static pthread_mutex_t reloadLock;
#define NAMELEN  500
#define CACHELEN 20
typedef struct
{
	char name[NAMELEN];
	uint16_t rid;
	ticks deadline;
} imagecache;
static imagecache remoteCloneCache[CACHELEN];

// ##########################################

static bool isForbiddenExtension(const char* name);
static dnbd3_image_t* image_remove(dnbd3_image_t *image);
static dnbd3_image_t* image_free(dnbd3_image_t *image);
static bool image_load_all_internal(char *base, char *path);
static bool image_addToList(dnbd3_image_t *image);
static bool image_load(char *base, char *path, int withUplink);
static bool image_clone(int sock, char *name, uint16_t revision, uint64_t imageSize);
static bool image_calcBlockCrc32(const int fd, const size_t block, const uint64_t realFilesize, uint32_t *crc);
static bool image_ensureDiskSpace(uint64_t size, bool force);

static uint8_t* image_loadCacheMap(const char * const imagePath, const int64_t fileSize);
static uint32_t* image_loadCrcList(const char * const imagePath, const int64_t fileSize, uint32_t *masterCrc);
static bool image_checkRandomBlocks(const int count, int fdImage, const int64_t fileSize, uint32_t * const crc32list, uint8_t * const cache_map);

// ##########################################

void image_serverStartup()
{
	srand( (unsigned int)time( NULL ) );
	mutex_init( &imageListLock );
	mutex_init( &remoteCloneLock );
	mutex_init( &reloadLock );
}

/**
 * Update cache-map of given image for the given byte range
 * start (inclusive) - end (exclusive)
 * Locks on: images[].lock
 */
void image_updateCachemap(dnbd3_image_t *image, uint64_t start, uint64_t end, const bool set)
{
	assert( image != NULL );
	// This should always be block borders due to how the protocol works, but better be safe
	// than accidentally mark blocks as cached when they really aren't entirely cached.
	assert( end <= image->virtualFilesize );
	assert( start <= end );
	if ( set ) {
		// If we set as cached, move "inwards" in case we're not at 4k border
		end &= ~(uint64_t)(DNBD3_BLOCK_SIZE - 1);
		start = (uint64_t)(start + DNBD3_BLOCK_SIZE - 1) & ~(uint64_t)(DNBD3_BLOCK_SIZE - 1);
	} else {
		// If marking as NOT cached, move "outwards" in case we're not at 4k border
		start &= ~(uint64_t)(DNBD3_BLOCK_SIZE - 1);
		end = (uint64_t)(end + DNBD3_BLOCK_SIZE - 1) & ~(uint64_t)(DNBD3_BLOCK_SIZE - 1);
	}
	if ( start >= end )
		return;
	bool setNewBlocks = false;
	uint64_t pos = start;
	mutex_lock( &image->lock );
	if ( image->cache_map == NULL ) {
		// Image seems already complete
		if ( set ) {
			// This makes no sense
			mutex_unlock( &image->lock );
			logadd( LOG_DEBUG1, "image_updateCachemap(true) with no cache_map: %s", image->path );
			return;
		}
		// Recreate a cache map, set it to all 1 initially as we assume the image was complete
		const int byteSize = IMGSIZE_TO_MAPBYTES( image->virtualFilesize );
		image->cache_map = malloc( byteSize );
		memset( image->cache_map, 0xff, byteSize );
	}
	while ( pos < end ) {
		const size_t map_y = (int)( pos >> 15 );
		const int map_x = (int)( (pos >> 12) & 7 ); // mod 8
		const int bit_mask = 1 << map_x;
		if ( set ) {
			if ( (image->cache_map[map_y] & bit_mask) == 0 ) setNewBlocks = true;
			image->cache_map[map_y] |= (uint8_t)bit_mask;
		} else {
			image->cache_map[map_y] &= (uint8_t)~bit_mask;
		}
		pos += DNBD3_BLOCK_SIZE;
	}
	if ( setNewBlocks && image->crc32 != NULL ) {
		// If setNewBlocks is set, at least one of the blocks was not cached before, so queue all hash blocks
		// for checking, even though this might lead to checking some hash block again, if it was
		// already complete and the block range spanned at least two hash blocks.
		// First set start and end to borders of hash blocks
		start &= ~(uint64_t)(HASH_BLOCK_SIZE - 1);
		end = (end + HASH_BLOCK_SIZE - 1) & ~(uint64_t)(HASH_BLOCK_SIZE - 1);
		pos = start;
		while ( pos < end ) {
			if ( image->cache_map == NULL ) break;
			const int block = (int)( pos / HASH_BLOCK_SIZE );
			if ( image_isHashBlockComplete( image->cache_map, block, image->realFilesize ) ) {
				mutex_unlock( &image->lock );
				integrity_check( image, block );
				mutex_lock( &image->lock );
			}
			pos += HASH_BLOCK_SIZE;
		}
	}
	mutex_unlock( &image->lock );
}

/**
 * Returns true if the given image is complete.
 * Also frees cache_map and deletes it on disk
 * if it hasn't been complete before
 * Locks on: image.lock
 */
bool image_isComplete(dnbd3_image_t *image)
{
	assert( image != NULL );
	mutex_lock( &image->lock );
	if ( image->virtualFilesize == 0 ) {
		mutex_unlock( &image->lock );
		return false;
	}
	if ( image->cache_map == NULL ) {
		mutex_unlock( &image->lock );
		return true;
	}
	bool complete = true;
	int j;
	const int map_len_bytes = IMGSIZE_TO_MAPBYTES( image->virtualFilesize );
	for (j = 0; j < map_len_bytes - 1; ++j) {
		if ( image->cache_map[j] != 0xFF ) {
			complete = false;
			break;
		}
	}
	if ( complete ) { // Every block except the last one is complete
		// Last one might need extra treatment if it's not a full byte
		const int blocks_in_last_byte = (image->virtualFilesize >> 12) & 7;
		uint8_t last_byte = 0;
		if ( blocks_in_last_byte == 0 ) {
			last_byte = 0xFF;
		} else {
			for (j = 0; j < blocks_in_last_byte; ++j)
				last_byte |= (uint8_t)(1 << j);
		}
		complete = ((image->cache_map[map_len_bytes - 1] & last_byte) == last_byte);
	}
	if ( !complete ) {
		mutex_unlock( &image->lock );
		return false;
	}
	char mapfile[PATHLEN] = "";
	free( image->cache_map );
	image->cache_map = NULL;
	snprintf( mapfile, PATHLEN, "%s.map", image->path );
	mutex_unlock( &image->lock );
	unlink( mapfile );
	return true;
}

/**
 * Make sure readFd is open, useful when closeUnusedFd is active.
 * This function assumes you called image_lock first, so its known
 * to be active and the fd won't be closed halfway through the
 * function.
 * Does not update atime, so the fd might be closed again very soon.
 * Since the caller should have image_lock()ed first, it could do
 * a quick operation on it before calling image_release which
 * guarantees that the fd will not be closed meanwhile.
 */
bool image_ensureOpen(dnbd3_image_t *image)
{
	if ( image->readFd != -1 ) return image;
	int newFd = open( image->path, O_RDONLY );
	if ( newFd != -1 ) {
		// Check size
		const off_t flen = lseek( newFd, 0, SEEK_END );
		if ( flen == -1 ) {
			logadd( LOG_WARNING, "Could not seek to end of %s (errno %d)", image->path, errno );
			close( newFd );
			newFd = -1;
		} else if ( (uint64_t)flen != image->realFilesize ) {
			logadd( LOG_WARNING, "Size of active image with closed fd changed from %" PRIu64 " to %" PRIu64, image->realFilesize, (uint64_t)flen );
			close( newFd );
			newFd = -1;
		}
	}
	if ( newFd == -1 ) {
		mutex_lock( &image->lock );
		image->working = false;
		mutex_unlock( &image->lock );
		return false;
	}
	mutex_lock( &image->lock );
	if ( image->readFd == -1 ) {
		image->readFd = newFd;
		mutex_unlock( &image->lock );
	} else {
		// There was a race while opening the file (happens cause not locked cause blocking), we lost the race so close new fd and proceed
		mutex_unlock( &image->lock );
		close( newFd );
	}
	return image->readFd != -1;
}

/**
 * Get an image by name+rid. This function increases a reference counter,
 * so you HAVE TO CALL image_release for every image_get() call at some
 * point...
 * Locks on: imageListLock, _images[].lock
 */
dnbd3_image_t* image_get(char *name, uint16_t revision, bool checkIfWorking)
{
	int i;
	const char *removingText = _removeMissingImages ? ", removing from list" : "";
	dnbd3_image_t *candidate = NULL;
	// Simple sanity check
	const size_t slen = strlen( name );
	if ( slen == 0 || name[slen - 1] == '/' || name[0] == '/' ) return NULL ;
	// Go through array
	mutex_lock( &imageListLock );
	for (i = 0; i < _num_images; ++i) {
		dnbd3_image_t * const image = _images[i];
		if ( image == NULL || strcmp( image->name, name ) != 0 ) continue;
		if ( revision == image->rid ) {
			candidate = image;
			break;
		} else if ( revision == 0 && (candidate == NULL || candidate->rid < image->rid) ) {
			candidate = image;
		}
	}

	// Not found
	if ( candidate == NULL ) {
		mutex_unlock( &imageListLock );
		return NULL ;
	}

	mutex_lock( &candidate->lock );
	mutex_unlock( &imageListLock );
	candidate->users++;
	mutex_unlock( &candidate->lock );

	// Found, see if it works
// TODO: Also make sure a non-working image still has old fd open but created a new one and removed itself from the list
// TODO: But remember size-changed images forever
	if ( candidate->working || checkIfWorking ) {
		// Is marked working, but might not have an fd open
		if ( !image_ensureOpen( candidate ) ) {
			mutex_lock( &candidate->lock );
			timing_get( &candidate->lastWorkCheck );
			mutex_unlock( &candidate->lock );
			if ( _removeMissingImages ) {
				candidate = image_remove( candidate ); // No release here, the image is still returned and should be released by caller
			}
			return candidate;
		}
	}

	if ( !checkIfWorking ) return candidate; // Not interested in re-cechking working state

	// ...not working...

	// Don't re-check too often
	mutex_lock( &candidate->lock );
	bool check;
	declare_now;
	check = timing_diff( &candidate->lastWorkCheck, &now ) > NONWORKING_RECHECK_INTERVAL_SECONDS;
	if ( check ) {
		candidate->lastWorkCheck = now;
	}
	mutex_unlock( &candidate->lock );
	if ( !check ) {
		return candidate;
	}

	// reaching this point means:
	// 1) We should check if the image is working, it might or might not be in working state right now
	// 2) The image is open for reading (or at least was at some point, the fd might be stale if images lie on an NFS share etc.)
	// 3) We made sure not to re-check this image too often

	// Common for ro and rw images: Size check, read check
	const off_t len = lseek( candidate->readFd, 0, SEEK_END );
	bool reload = false;
	if ( len == -1 ) {
		logadd( LOG_WARNING, "lseek() on %s failed (errno=%d)%s.", candidate->path, errno, removingText );
		reload = true;
	} else if ( (uint64_t)len != candidate->realFilesize ) {
		logadd( LOG_DEBUG1, "Size of %s changed at runtime, keeping disabled! Expected: %" PRIu64 ", found: %" PRIu64
				". Try sending SIGHUP to server if you know what you're doing.",
				candidate->path, candidate->realFilesize, (uint64_t)len );
	} else {
		// Seek worked, file size is same, now see if we can read from file
		char buffer[100];
		if ( pread( candidate->readFd, buffer, sizeof(buffer), 0 ) == -1 ) {
			logadd( LOG_DEBUG2, "Reading first %d bytes from %s failed (errno=%d)%s.",
					(int)sizeof(buffer), candidate->path, errno, removingText );
			reload = true;
		} else if ( !candidate->working ) {
			// Seems everything is fine again \o/
			candidate->working = true;
			logadd( LOG_INFO, "Changed state of %s:%d to 'working'", candidate->name, candidate->rid );
		}
	}

	if ( reload ) {
		// Could not access the image with exising fd - mark for reload which will re-open the file.
		// make a copy of the image struct but keep the old one around. If/When it's not being used
		// anymore, it will be freed automatically.
		dnbd3_image_t *img = calloc( sizeof(dnbd3_image_t), 1 );
		img->path = strdup( candidate->path );
		img->name = strdup( candidate->name );
		img->virtualFilesize = candidate->virtualFilesize;
		img->realFilesize = candidate->realFilesize;
		img->atime = now;
		img->masterCrc32 = candidate->masterCrc32;
		img->readFd = -1;
		img->rid = candidate->rid;
		img->users = 1;
		img->working = false;
		mutex_init( &img->lock );
		if ( candidate->crc32 != NULL ) {
			const size_t mb = IMGSIZE_TO_HASHBLOCKS( candidate->virtualFilesize ) * sizeof(uint32_t);
			img->crc32 = malloc( mb );
			memcpy( img->crc32, candidate->crc32, mb );
		}
		mutex_lock( &candidate->lock );
		if ( candidate->cache_map != NULL ) {
			const size_t mb = IMGSIZE_TO_MAPBYTES( candidate->virtualFilesize );
			img->cache_map = malloc( mb );
			memcpy( img->cache_map, candidate->cache_map, mb );
		}
		mutex_unlock( &candidate->lock );
		if ( image_addToList( img ) ) {
			image_release( candidate );
			candidate = img;
		} else {
			img->users = 0;
			image_free( img );
		}
		// readFd == -1 and working == FALSE at this point,
		// this function needs some splitting up for handling as we need to run most
		// of the above code again. for now we know that the next call for this
		// name:rid will get ne newly inserted "img" and try to re-open the file.
	}

	// Check if image is incomplete, handle
	if ( candidate->cache_map != NULL ) {
		if ( candidate->uplink == NULL ) {
			uplink_init( candidate, -1, NULL, -1 );
		}
	}

	return candidate; // We did all we can, hopefully it's working
}

/**
 * Lock the image by increasing its users count
 * Returns the image on success, NULL if it is not found in the image list
 * Every call to image_lock() needs to be followed by a call to image_release() at some point.
 * Locks on: imageListLock, _images[].lock
 */
dnbd3_image_t* image_lock(dnbd3_image_t *image) // TODO: get rid, fix places that do image->users--
{
	if ( image == NULL ) return NULL ;
	int i;
	mutex_lock( &imageListLock );
	for (i = 0; i < _num_images; ++i) {
		if ( _images[i] == image ) {
			mutex_lock( &image->lock );
			mutex_unlock( &imageListLock );
			image->users++;
			mutex_unlock( &image->lock );
			return image;
		}
	}
	mutex_unlock( &imageListLock );
	return NULL ;
}

/**
 * Release given image. This will decrease the reference counter of the image.
 * If the usage counter reaches 0 and the image is not in the images array
 * anymore, the image will be freed
 * Locks on: imageListLock, _images[].lock
 */
dnbd3_image_t* image_release(dnbd3_image_t *image)
{
	if ( image == NULL ) return NULL;
	mutex_lock( &imageListLock );
	mutex_lock( &image->lock );
	assert( image->users > 0 );
	image->users--;
	bool inUse = image->users != 0;
	mutex_unlock( &image->lock );
	if ( inUse ) { // Still in use, do nothing
		mutex_unlock( &imageListLock );
		return NULL;
	}
	// Getting here means we decreased the usage counter to zero
	// If the image is not in the images list anymore, we're
	// responsible for freeing it
	for (int i = 0; i < _num_images; ++i) {
		if ( _images[i] == image ) { // Found, do nothing
			mutex_unlock( &imageListLock );
			return NULL;
		}
	}
	mutex_unlock( &imageListLock );
	// So it wasn't in the images list anymore either, get rid of it
	if ( !inUse ) image = image_free( image );
	return NULL;
}

/**
 * Returns true if the given file name ends in one of our meta data
 * file extensions. Used to prevent loading them as images.
 */
static bool isForbiddenExtension(const char* name)
{
	const size_t len = strlen( name );
	if ( len < 4 ) return false;
	const char *ptr = name + len - 4;
	if ( strcmp( ptr, ".crc" ) == 0 ) return true; // CRC list
	if ( strcmp( ptr, ".map" ) == 0 ) return true; // cache map for incomplete images
	if ( len < 5 ) return false;
	--ptr;
	if ( strcmp( ptr, ".meta" ) == 0 ) return true; // Meta data (currently not in use)
	return false;
}

/**
 * Remove image from images array. Only free it if it has
 * no active users and was actually in the list.
 * Locks on: imageListLock, image[].lock
 * @return NULL if image was also freed, image otherwise
 */
static dnbd3_image_t* image_remove(dnbd3_image_t *image)
{
	bool mustFree = false;
	mutex_lock( &imageListLock );
	mutex_lock( &image->lock );
	for ( int i = _num_images - 1; i >= 0; --i ) {
		if ( _images[i] == image ) {
			_images[i] = NULL;
			mustFree = ( image->users == 0 );
		}
		if ( _images[i] == NULL && i + 1 == _num_images ) _num_images--;
	}
	mutex_unlock( &image->lock );
	mutex_unlock( &imageListLock );
	if ( mustFree ) image = image_free( image );
	return image;
}

/**
 * Kill all uplinks
 */
void image_killUplinks()
{
	int i;
	mutex_lock( &imageListLock );
	for (i = 0; i < _num_images; ++i) {
		if ( _images[i] == NULL ) continue;
		mutex_lock( &_images[i]->lock );
		if ( _images[i]->uplink != NULL ) {
			mutex_lock( &_images[i]->uplink->queueLock );
			if ( !_images[i]->uplink->shutdown ) {
				thread_detach( _images[i]->uplink->thread );
				_images[i]->uplink->shutdown = true;
			}
			mutex_unlock( &_images[i]->uplink->queueLock );
			signal_call( _images[i]->uplink->signal );
		}
		mutex_unlock( &_images[i]->lock );
	}
	mutex_unlock( &imageListLock );
}

/**
 * Load all images in given path recursively.
 * Pass NULL to use path from config.
 */
bool image_loadAll(char *path)
{
	bool ret;
	char imgPath[PATHLEN];
	int imgId;
	dnbd3_image_t *imgHandle;

	if ( path == NULL ) path = _basePath;
	if ( mutex_trylock( &reloadLock ) != 0 ) {
		logadd( LOG_MINOR, "Could not (re)load image list, already in progress." );
		return false;
	}
	if ( _removeMissingImages ) {
		// Check if all loaded images still exist on disk
		logadd( LOG_INFO, "Checking for vanished images" );
		mutex_lock( &imageListLock );
		for ( int i = _num_images - 1; i >= 0; --i ) {
			if ( _shutdown ) break;
			if ( _images[i] == NULL ) {
				if ( i + 1 == _num_images ) _num_images--;
				continue;
			}
			imgId = _images[i]->id;
			snprintf( imgPath, PATHLEN, "%s", _images[i]->path );
			mutex_unlock( &imageListLock ); // isReadable hits the fs; unlock
			// Check if fill can still be opened for reading
			ret = file_isReadable( imgPath );
			// Lock again, see if image is still there, free if required
			mutex_lock( &imageListLock );
			if ( ret || i >= _num_images || _images[i] == NULL || _images[i]->id != imgId ) continue;
			// Image needs to be removed
			imgHandle = _images[i];
			_images[i] = NULL;
			if ( i + 1 == _num_images ) _num_images--;
			mutex_lock( &imgHandle->lock );
			const bool freeImg = ( imgHandle->users == 0 );
			mutex_unlock( &imgHandle->lock );
			// We unlocked, but the image has been removed from the list already, so
			// there's no way the users-counter can increase at this point.
			if ( freeImg ) {
				// Image is not in use anymore, free the dangling entry immediately
				mutex_unlock( &imageListLock ); // image_free might do several fs operations; unlock
				image_free( imgHandle );
				mutex_lock( &imageListLock );
			}
		}
		mutex_unlock( &imageListLock );
		if ( _shutdown ) {
			mutex_unlock( &reloadLock );
			return true;
		}
	}
	// Now scan for new images
	logadd( LOG_INFO, "Scanning for new or modified images" );
	ret = image_load_all_internal( path, path );
	mutex_unlock( &reloadLock );
	logadd( LOG_INFO, "Finished scanning %s", path );
	return ret;
}

/**
 * Free all images we have, but only if they're not in use anymore.
 * Locks on imageListLock
 * @return true if all images have been freed
 */
bool image_tryFreeAll()
{
	mutex_lock( &imageListLock );
	for (int i = _num_images - 1; i >= 0; --i) {
		if ( _images[i] != NULL && _images[i]->users == 0 ) { // XXX Data race...
			dnbd3_image_t *image = _images[i];
			_images[i] = NULL;
			mutex_unlock( &imageListLock );
			image = image_free( image );
			mutex_lock( &imageListLock );
		}
		if ( i + 1 == _num_images && _images[i] == NULL ) _num_images--;
	}
	mutex_unlock( &imageListLock );
	return _num_images == 0;
}

/**
 * Free image. DOES NOT check if it's in use.
 * Indirectly locks on imageListLock, image.lock, uplink.queueLock
 */
static dnbd3_image_t* image_free(dnbd3_image_t *image)
{
	assert( image != NULL );
	if ( !_shutdown ) {
		logadd( LOG_INFO, "Freeing image %s:%d", image->name, (int)image->rid );
	}
	//
	uplink_shutdown( image );
	mutex_lock( &image->lock );
	free( image->cache_map );
	free( image->crc32 );
	free( image->path );
	free( image->name );
	image->cache_map = NULL;
	image->crc32 = NULL;
	image->path = NULL;
	image->name = NULL;
	mutex_unlock( &image->lock );
	if ( image->readFd != -1 ) close( image->readFd );
	mutex_destroy( &image->lock );
	//
	memset( image, 0, sizeof(*image) );
	free( image );
	return NULL ;
}

bool image_isHashBlockComplete(const uint8_t * const cacheMap, const uint64_t block, const uint64_t realFilesize)
{
	if ( cacheMap == NULL ) return true;
	const uint64_t end = (block + 1) * HASH_BLOCK_SIZE;
	if ( end <= realFilesize ) {
		// Trivial case: block in question is not the last block (well, or image size is multiple of HASH_BLOCK_SIZE)
		const int startCacheIndex = (int)( ( block * HASH_BLOCK_SIZE ) / ( DNBD3_BLOCK_SIZE * 8 ) );
		const int endCacheIndex = startCacheIndex + (int)( HASH_BLOCK_SIZE / ( DNBD3_BLOCK_SIZE * 8 ) );
		for ( int i = startCacheIndex; i < endCacheIndex; ++i ) {
			if ( cacheMap[i] != 0xff ) {
				return false;
			}
		}
	} else {
		// Special case: Checking last block, which is smaller than HASH_BLOCK_SIZE
		for (uint64_t mapPos = block * HASH_BLOCK_SIZE; mapPos < realFilesize; mapPos += DNBD3_BLOCK_SIZE ) {
			const size_t map_y = (size_t)( mapPos >> 15 );
			const int map_x = (int)( (mapPos >> 12) & 7 ); // mod 8
			const int mask = 1 << map_x;
			if ( (cacheMap[map_y] & mask) == 0 ) return false;
		}
	}
	return true;
}

/**
 * Load all images in the given path recursively,
 * consider *base the base path that is to be cut off
 */
static bool image_load_all_internal(char *base, char *path)
{
#define SUBDIR_LEN 150
	assert( path != NULL );
	assert( *path == '/' );
	struct dirent entry, *entryPtr;
	const size_t pathLen = strlen( path );
	char subpath[PATHLEN];
	struct stat st;
	DIR * const dir = opendir( path );

	if ( dir == NULL ) {
		logadd( LOG_ERROR, "Could not opendir '%s' for loading", path );
		return false;
	}

	while ( !_shutdown && (entryPtr = readdir( dir )) != NULL ) {
		entry = *entryPtr;
		if ( strcmp( entry.d_name, "." ) == 0 || strcmp( entry.d_name, ".." ) == 0 ) continue;
		if ( strlen( entry.d_name ) > SUBDIR_LEN ) {
			logadd( LOG_WARNING, "Skipping entry %s: Too long (max %d bytes)", entry.d_name, (int)SUBDIR_LEN );
			continue;
		}
		if ( entry.d_name[0] == '/' || path[pathLen - 1] == '/' ) {
			snprintf( subpath, PATHLEN, "%s%s", path, entry.d_name );
		} else {
			snprintf( subpath, PATHLEN, "%s/%s", path, entry.d_name );
		}
		if ( stat( subpath, &st ) < 0 ) {
			logadd( LOG_WARNING, "stat() for '%s' failed. Ignoring....", subpath );
			continue;
		}
		if ( S_ISDIR( st.st_mode ) ) {
			image_load_all_internal( base, subpath ); // Recurse
		} else if ( !isForbiddenExtension( subpath ) ) {
			image_load( base, subpath, true ); // Load image if possible
		}
	}
	closedir( dir );
	return true;
#undef SUBDIR_LEN
}

/**
 */
static bool image_addToList(dnbd3_image_t *image)
{
	int i;
	static int imgIdCounter = 0; // Used to assign unique numeric IDs to images
	mutex_lock( &imageListLock );
	// Now we're locked, assign unique ID to image (unique for this running server instance!)
	image->id = ++imgIdCounter;
	for ( i = 0; i < _num_images; ++i ) {
		if ( _images[i] != NULL ) continue;
		_images[i] = image;
		break;
	}
	if ( i >= _num_images ) {
		if ( _num_images >= _maxImages ) {
			mutex_unlock( &imageListLock );
			return false;
		}
		_images[_num_images++] = image;
	}
	mutex_unlock( &imageListLock );
	return true;
}

/**
 * Load image from given path. This will check if the image is
 * already loaded and updates its information in that case.
 * Note that this is NOT THREAD SAFE so make sure its always
 * called on one thread only.
 */
static bool image_load(char *base, char *path, int withUplink)
{
	int revision = -1;
	struct stat st;
	uint8_t *cache_map = NULL;
	uint32_t *crc32list = NULL;
	dnbd3_image_t *existing = NULL;
	int fdImage = -1;
	bool function_return = false; // Return false by default
	assert( base != NULL );
	assert( path != NULL );
	assert( *path == '/' );
	assert( strncmp( path, base, strlen(base)) == 0 );
	assert( base[strlen(base) - 1] != '/' );
	assert( strlen(path) > strlen(base) );
	char *lastSlash = strrchr( path, '/' );
	char *fileName = lastSlash + 1;
	char imgName[strlen( path )];
	const size_t fileNameLen = strlen( fileName );

	// Copy virtual path (relative path in "base")
	char * const virtBase = path + strlen( base ) + 1;
	assert( *virtBase != '/' );
	char *src = virtBase, *dst = imgName;
	while ( src <= lastSlash ) {
		*dst++ = *src++;
	}
	*dst = '\0';

	do {
		// Parse file name for revision
		// Try to parse *.r<ID> syntax
		size_t i;
		for (i = fileNameLen - 1; i > 1; --i) {
			if ( fileName[i] < '0' || fileName[i] > '9' ) break;
		}
		if ( i != fileNameLen - 1 && fileName[i] == 'r' && fileName[i - 1] == '.' ) {
			revision = atoi( fileName + i + 1 );
			src = fileName;
			while ( src < fileName + i - 1 ) {
				*dst++ = *src++;
			}
			*dst = '\0';
		}
	} while (0);

	// Legacy mode enabled and no rid extracted from filename?
	if ( _vmdkLegacyMode && revision == -1 ) {
		fdImage = open( path, O_RDONLY ); // Check if it exists
		if ( fdImage == -1 ) goto load_error;
		// Yes, simply append full file name and set rid to 1
		strcat( dst, fileName );
		revision = 1;
	}
	// Did we get anything?
	if ( revision <= 0 || revision >= 65536 ) {
		logadd( LOG_WARNING, "Image '%s' has invalid revision ID %d", path, revision );
		goto load_error;
	}

	// Get pointer to already existing image if possible
	existing = image_get( imgName, (uint16_t)revision, true );

	// ### Now load the actual image related data ###
	if ( fdImage == -1 ) {
		fdImage = open( path, O_RDONLY );
	}
	if ( fdImage == -1 ) {
		logadd( LOG_ERROR, "Could not open '%s' for reading...", path );
		goto load_error;
	}
	// Determine file size
	const off_t seekret = lseek( fdImage, 0, SEEK_END );
	if ( seekret < 0 ) {
		logadd( LOG_ERROR, "Could not seek to end of file '%s'", path );
		goto load_error;
	} else if ( seekret == 0 ) {
		logadd( LOG_WARNING, "Empty image file '%s'", path );
		goto load_error;
	}
	const uint64_t realFilesize = (uint64_t)seekret;
	const uint64_t virtualFilesize = ( realFilesize + (DNBD3_BLOCK_SIZE - 1) ) & ~(DNBD3_BLOCK_SIZE - 1);
	if ( realFilesize != virtualFilesize ) {
		logadd( LOG_DEBUG1, "Image size of '%s' is %" PRIu64 ", virtual size: %" PRIu64, path, realFilesize, virtualFilesize );
	}

	// 1. Allocate memory for the cache map if the image is incomplete
	cache_map = image_loadCacheMap( path, virtualFilesize );

	// XXX: Maybe try sha-256 or 512 first if you're paranoid (to be implemented)

	// 2. Load CRC-32 list of image
	bool doFullCheck = false;
	uint32_t masterCrc = 0;
	const int hashBlockCount = IMGSIZE_TO_HASHBLOCKS( virtualFilesize );
	crc32list = image_loadCrcList( path, virtualFilesize, &masterCrc );

	// Check CRC32
	if ( crc32list != NULL ) {
		if ( !image_checkRandomBlocks( 4, fdImage, realFilesize, crc32list, cache_map ) ) {
			logadd( LOG_ERROR, "quick crc32 check of %s failed. Data corruption?", path );
			doFullCheck = true;
		}
	}

	// Compare data just loaded to identical image we apparently already loaded
	if ( existing != NULL ) {
		if ( existing->realFilesize != realFilesize ) {
			logadd( LOG_WARNING, "Size of image '%s:%d' has changed.", existing->name, (int)existing->rid );
			// Image will be replaced below
		} else if ( existing->crc32 != NULL && crc32list != NULL
				&& memcmp( existing->crc32, crc32list, sizeof(uint32_t) * hashBlockCount ) != 0 ) {
			logadd( LOG_WARNING, "CRC32 list of image '%s:%d' has changed.", existing->name, (int)existing->rid );
			logadd( LOG_WARNING, "The image will be reloaded, but you should NOT replace existing images while the server is running." );
			logadd( LOG_WARNING, "Actually even if it's not running this should never be done. Use a new RID instead!" );
			// Image will be replaced below
		} else if ( existing->crc32 == NULL && crc32list != NULL ) {
			logadd( LOG_INFO, "Found CRC-32 list for already loaded image '%s:%d', adding...", existing->name, (int)existing->rid );
			existing->crc32 = crc32list;
			existing->masterCrc32 = masterCrc;
			crc32list = NULL;
			function_return = true;
			goto load_error; // Keep existing
		} else if ( existing->cache_map != NULL && cache_map == NULL ) {
			// Just ignore that fact, if replication is really complete the cache map will be removed anyways
			logadd( LOG_INFO, "Image '%s:%d' has no cache map on disk!", existing->name, (int)existing->rid );
			function_return = true;
			goto load_error; // Keep existing
		} else {
			// Nothing changed about the existing image, so do nothing
			logadd( LOG_DEBUG1, "Did not change" );
			function_return = true;
			goto load_error; // Keep existing
		}
		// Remove existing image from images array, so it will be replaced by the reloaded image
		existing = image_remove( existing );
		existing = image_release( existing );
	}

	// Load fresh image
	dnbd3_image_t *image = calloc( 1, sizeof(dnbd3_image_t) );
	image->path = strdup( path );
	image->name = strdup( imgName );
	image->cache_map = cache_map;
	image->crc32 = crc32list;
	image->masterCrc32 = masterCrc;
	image->uplink = NULL;
	image->realFilesize = realFilesize;
	image->virtualFilesize = virtualFilesize;
	image->rid = (uint16_t)revision;
	image->users = 0;
	image->readFd = -1;
	image->working = (image->cache_map == NULL );
	timing_get( &image->nextCompletenessEstimate );
	image->completenessEstimate = -1;
	mutex_init( &image->lock );
	int32_t offset;
	if ( stat( path, &st ) == 0 ) {
		// Negatively offset atime by file modification time
		offset = (int32_t)( st.st_mtime - time( NULL ) );
		if ( offset > 0 ) offset = 0;
	} else {
		offset = 0;
	}
	timing_gets( &image->atime, offset );

	// Prevent freeing in cleanup
	cache_map = NULL;
	crc32list = NULL;

	// Get rid of cache map if image is complete
	if ( image->cache_map != NULL ) {
		image_isComplete( image );
	}

	// Image is definitely incomplete, initialize uplink worker
	if ( image->cache_map != NULL ) {
		image->working = false;
		if ( withUplink ) {
			uplink_init( image, -1, NULL, -1 );
		}
	}

	// ### Reaching this point means loading succeeded
	image->readFd = fdImage;
	if ( image_addToList( image ) ) {
		// Keep fd for reading
		fdImage = -1;
	} else {
		logadd( LOG_ERROR, "Image list full: Could not add image %s", path );
		image->readFd = -1; // Keep fdImage instead, will be closed below
		image = image_free( image );
		goto load_error;
	}
	logadd( LOG_DEBUG1, "Loaded image '%s:%d'\n", image->name, (int)image->rid );
	// CRC errors found...
	if ( doFullCheck ) {
		logadd( LOG_INFO, "Queueing full CRC32 check for '%s:%d'\n", image->name, (int)image->rid );
		integrity_check( image, -1 );
	}

	function_return = true;

	// Clean exit:
load_error: ;
	if ( existing != NULL ) existing = image_release( existing );
	if ( crc32list != NULL ) free( crc32list );
	if ( cache_map != NULL ) free( cache_map );
	if ( fdImage != -1 ) close( fdImage );
	return function_return;
}

static uint8_t* image_loadCacheMap(const char * const imagePath, const int64_t fileSize)
{
	uint8_t *retval = NULL;
	char mapFile[strlen( imagePath ) + 10 + 1];
	sprintf( mapFile, "%s.map", imagePath );
	int fdMap = open( mapFile, O_RDONLY );
	if ( fdMap >= 0 ) {
		const int map_size = IMGSIZE_TO_MAPBYTES( fileSize );
		retval = calloc( 1, map_size );
		const ssize_t rd = read( fdMap, retval, map_size );
		if ( map_size != rd ) {
			logadd( LOG_WARNING, "Could only read %d of expected %d bytes of cache map of '%s'", (int)rd, (int)map_size, imagePath );
			// Could not read complete map, that means the rest of the image file will be considered incomplete
		}
		close( fdMap );
		// Later on we check if the hash map says the image is complete
	}
	return retval;
}

static uint32_t* image_loadCrcList(const char * const imagePath, const int64_t fileSize, uint32_t *masterCrc)
{
	assert( masterCrc != NULL );
	uint32_t *retval = NULL;
	const int hashBlocks = IMGSIZE_TO_HASHBLOCKS( fileSize );
	// Currently this should only prevent accidental corruption (esp. regarding transparent proxy mode)
	// but maybe later on you want better security
	char hashFile[strlen( imagePath ) + 10 + 1];
	sprintf( hashFile, "%s.crc", imagePath );
	int fdHash = open( hashFile, O_RDONLY );
	if ( fdHash >= 0 ) {
		off_t fs = lseek( fdHash, 0, SEEK_END );
		if ( fs < (hashBlocks + 1) * 4 ) {
			logadd( LOG_WARNING, "Ignoring crc32 list for '%s' as it is too short", imagePath );
		} else {
			if ( pread( fdHash, masterCrc, sizeof(uint32_t), 0 ) != sizeof(uint32_t) ) {
				logadd( LOG_WARNING, "Error reading first crc32 of '%s'", imagePath );
			} else {
				const size_t crcFileLen = hashBlocks * sizeof(uint32_t);
				size_t pos = 0;
				retval = calloc( hashBlocks, sizeof(uint32_t) );
				while ( pos < crcFileLen ) {
					ssize_t ret = pread( fdHash, retval + pos, crcFileLen - pos, pos + sizeof(uint32_t) /* skip master-crc */ );
					if ( ret == -1 ) {
						if ( errno == EINTR || errno == EAGAIN ) continue;
					}
					if ( ret <= 0 ) break;
					pos += ret;
				}
				if ( pos != crcFileLen ) {
					free( retval );
					retval = NULL;
					logadd( LOG_WARNING, "Could not read crc32 list of '%s'", imagePath );
				} else {
					uint32_t lists_crc = crc32( 0, NULL, 0 );
					lists_crc = crc32( lists_crc, (uint8_t*)retval, hashBlocks * sizeof(uint32_t) );
					lists_crc = net_order_32( lists_crc );
					if ( lists_crc != *masterCrc ) {
						free( retval );
						retval = NULL;
						logadd( LOG_WARNING, "CRC-32 of CRC-32 list mismatch. CRC-32 list of '%s' might be corrupted.", imagePath );
					}
				}
			}
		}
		close( fdHash );
	}
	return retval;
}

static bool image_checkRandomBlocks(const int count, int fdImage, const int64_t realFilesize, uint32_t * const crc32list, uint8_t * const cache_map)
{
	// This checks the first block and (up to) count - 1 random blocks for corruption
	// via the known crc32 list. This is very sloppy and is merely supposed to detect
	// accidental corruption due to broken dnbd3-proxy functionality or file system
	// corruption.
	assert( count > 0 );
	const int hashBlocks = IMGSIZE_TO_HASHBLOCKS( realFilesize );
	int blocks[count + 1];
	int index = 0, j;
	int block;
	if ( image_isHashBlockComplete( cache_map, 0, realFilesize ) ) blocks[index++] = 0;
	int tries = count * 5; // Try only so many times to find a non-duplicate complete block
	while ( index + 1 < count && --tries > 0 ) {
		block = rand() % hashBlocks; // Random block
		for ( j = 0; j < index; ++j ) { // Random block already in list?
			if ( blocks[j] == block ) goto while_end;
		}
		// Block complete? If yes, add to list
		if ( image_isHashBlockComplete( cache_map, block, realFilesize ) ) blocks[index++] = block;
while_end: ;
	}
	blocks[MIN(index, count)] = -1; // End of array has to be marked by a -1
	return image_checkBlocksCrc32( fdImage, crc32list, blocks, realFilesize ); // Return result of check
}

/**
 * Create a new image with the given image name and revision id in _basePath
 * Returns true on success, false otherwise
 */
bool image_create(char *image, int revision, uint64_t size)
{
	assert( image != NULL );
	assert( size >= DNBD3_BLOCK_SIZE );
	if ( revision <= 0 ) {
		logadd( LOG_ERROR, "revision id invalid: %d", revision );
		return false;
	}
	char path[PATHLEN], cache[PATHLEN];
	char *lastSlash = strrchr( image, '/' );
	if ( lastSlash == NULL ) {
		snprintf( path, PATHLEN, "%s/%s.r%d", _basePath, image, revision );
	} else {
		*lastSlash = '\0';
		snprintf( path, PATHLEN, "%s/%s", _basePath, image );
		mkdir_p( path );
		*lastSlash = '/';
		snprintf( path, PATHLEN, "%s/%s.r%d", _basePath, image, revision );
	}
	snprintf( cache, PATHLEN, "%s.map", path );
	size = (size + DNBD3_BLOCK_SIZE - 1) & ~(uint64_t)(DNBD3_BLOCK_SIZE - 1);
	const int mapsize = IMGSIZE_TO_MAPBYTES(size);
	// Write files
	int fdImage = -1, fdCache = -1;
	fdImage = open( path, O_RDWR | O_TRUNC | O_CREAT, 0644 );
	fdCache = open( cache, O_RDWR | O_TRUNC | O_CREAT, 0644 );
	if ( fdImage < 0 ) {
		logadd( LOG_ERROR, "Could not open %s for writing.", path );
		goto failure_cleanup;
	}
	if ( fdCache < 0 ) {
		logadd( LOG_ERROR, "Could not open %s for writing.", cache );
		goto failure_cleanup;
	}
	// Try cache map first
	if ( !file_alloc( fdCache, 0, mapsize ) && !file_setSize( fdCache, mapsize ) ) {
		const int err = errno;
		logadd( LOG_DEBUG1, "Could not allocate %d bytes for %s (errno=%d)", mapsize, cache, err );
	}
	// Now write image
	if ( !_sparseFiles && !file_alloc( fdImage, 0, size ) ) {
		logadd( LOG_ERROR, "Could not allocate %" PRIu64 " bytes for %s (errno=%d)", size, path, errno );
		logadd( LOG_ERROR, "It is highly recommended to use a file system that supports preallocating disk"
				" space without actually writing all zeroes to the block device." );
		logadd( LOG_ERROR, "If you cannot fix this, try setting sparseFiles=true, but don't expect"
				" divine performance during replication." );
		goto failure_cleanup;
	} else if ( _sparseFiles && !file_setSize( fdImage, size ) ) {
		logadd( LOG_ERROR, "Could not create sparse file of %" PRIu64 " bytes for %s (errno=%d)", size, path, errno );
		logadd( LOG_ERROR, "Make sure you have enough disk space, check directory permissions, fs errors etc." );
		goto failure_cleanup;
	}
	close( fdImage );
	close( fdCache );
	return true;
	//
failure_cleanup: ;
	if ( fdImage >= 0 ) close( fdImage );
	if ( fdCache >= 0 ) close( fdCache );
	remove( path );
	remove( cache );
	return false;
}

static dnbd3_image_t *loadImageProxy(char * const name, const uint16_t revision, const size_t len);
static dnbd3_image_t *loadImageServer(char * const name, const uint16_t requestedRid);

/**
 * Does the same as image_get, but if the image is not known locally, or if
 * revision 0 is requested, it will:
 * a) Try to clone it from an authoritative dnbd3 server, if
 *    the server is running in proxy mode.
 * b) Try to load it from disk by constructing the appropriate file name, if not
 *    running in proxy mode.
 *
 *  If the return value is not NULL,
 * image_release needs to be called on the image at some point.
 * Locks on: remoteCloneLock, imageListLock, _images[].lock
 */
dnbd3_image_t* image_getOrLoad(char * const name, const uint16_t revision)
{
	// specific revision - try shortcut
	if ( revision != 0 ) {
		dnbd3_image_t *image = image_get( name, revision, true );
		if ( image != NULL ) return image;
	}
	const size_t len = strlen( name );
	// Sanity check
	if ( len == 0 || name[len - 1] == '/' || name[0] == '/'
			|| name[0] == '.' || strstr( name, "/." ) != NULL ) return NULL;
	// Call specific function depending on whether this is a proxy or not
	if ( _isProxy ) {
		return loadImageProxy( name, revision, len );
	} else {
		return loadImageServer( name, revision );
	}
}

/**
 * Called if specific rid is not loaded, or if rid is 0 (some version might be loaded locally,
 * but we should check if there's a higher rid on a remote server).
 */
static dnbd3_image_t *loadImageProxy(char * const name, const uint16_t revision, const size_t len)
{
	// Already existing locally?
	dnbd3_image_t *image = NULL;
	if ( revision == 0 ) {
		image = image_get( name, revision, true );
	}

	// Doesn't exist or is rid 0, try remote if not already tried it recently
	declare_now;
	char *cmpname = name;
	int useIndex = -1, fallbackIndex = 0;
	if ( len >= NAMELEN ) cmpname += 1 + len - NAMELEN;
	mutex_lock( &remoteCloneLock );
	for (int i = 0; i < CACHELEN; ++i) {
		if ( remoteCloneCache[i].rid == revision && strcmp( cmpname, remoteCloneCache[i].name ) == 0 ) {
			useIndex = i;
			if ( timing_reached( &remoteCloneCache[i].deadline, &now ) ) break;
			mutex_unlock( &remoteCloneLock ); // Was recently checked...
			return image;
		}
		if ( timing_1le2( &remoteCloneCache[i].deadline, &remoteCloneCache[fallbackIndex].deadline ) ) {
			fallbackIndex = i;
		}
	}
	// Re-check to prevent two clients at the same time triggering this,
	// but only if rid != 0, since we would just get an old rid then
	if ( revision != 0 ) {
		if ( image == NULL ) image = image_get( name, revision, true );
		if ( image != NULL ) {
			mutex_unlock( &remoteCloneLock );
			return image;
		}
	}
	// Reaching this point means we should contact an authority server
	serialized_buffer_t serialized;
	// Mark as recently checked
	if ( useIndex == -1 ) {
		useIndex = fallbackIndex;
	}
	timing_set( &remoteCloneCache[useIndex].deadline, &now, SERVER_REMOTE_IMAGE_CHECK_CACHETIME );
	snprintf( remoteCloneCache[useIndex].name, NAMELEN, "%s", cmpname );
	remoteCloneCache[useIndex].rid = revision;
	mutex_unlock( &remoteCloneLock );

	// Get some alt servers and try to get the image from there
#define REP_NUM_SRV (8)
	dnbd3_host_t servers[REP_NUM_SRV];
	int uplinkSock = -1;
	dnbd3_host_t uplinkServer;
	const int count = altservers_getListForUplink( servers, REP_NUM_SRV, false );
	uint16_t remoteProtocolVersion;
	uint16_t remoteRid = revision;
	uint64_t remoteImageSize;
	struct sockaddr_storage sa;
	socklen_t salen;
	poll_list_t *cons = sock_newPollList();
	logadd( LOG_DEBUG2, "Trying to clone %s:%d from %d hosts", name, (int)revision, count );
	for (int i = 0; i < count + 5; ++i) { // "i < count + 5" for 5 additional iterations, waiting on pending connects
		char *remoteName;
		bool ok = false;
		int sock;
		if ( i >= count ) {
			sock = sock_multiConnect( cons, NULL, 100, 1000 );
			if ( sock == -2 ) break;
		} else {
			if ( log_hasMask( LOG_DEBUG2 ) ) {
				char host[50];
				size_t len = sock_printHost( &servers[i], host, sizeof(host) );
				host[len] = '\0';
				logadd( LOG_DEBUG2, "Trying to replicate from %s", host );
			}
			sock = sock_multiConnect( cons, &servers[i], 100, 1000 );
		}
		if ( sock == -1 || sock == -2 ) continue;
		salen = sizeof(sa);
		if ( getpeername( sock, (struct sockaddr*)&sa, &salen ) == -1 ) {
			logadd( LOG_MINOR, "getpeername on successful connection failed!? (errno=%d)", errno );
			goto server_fail;
		}
		if ( !dnbd3_select_image( sock, name, revision, SI_SERVER_FLAGS ) ) goto server_fail;
		if ( !dnbd3_select_image_reply( &serialized, sock, &remoteProtocolVersion, &remoteName, &remoteRid, &remoteImageSize ) ) goto server_fail;
		if ( remoteProtocolVersion < MIN_SUPPORTED_SERVER || remoteRid == 0 ) goto server_fail;
		if ( revision != 0 && remoteRid != revision ) goto server_fail; // Want specific revision but uplink supplied different rid
		if ( revision == 0 && image != NULL && image->rid >= remoteRid ) goto server_fail; // Not actually a failure: Highest remote rid is <= highest local rid - don't clone!
		if ( remoteImageSize < DNBD3_BLOCK_SIZE || remoteName == NULL || strcmp( name, remoteName ) != 0 ) goto server_fail;
		if ( remoteImageSize > _maxReplicationSize ) {
			logadd( LOG_MINOR, "Won't proxy '%s:%d': Larger than maxReplicationSize", name, (int)revision );
			goto server_fail;
		}
		mutex_lock( &reloadLock );
		// Ensure disk space entirely if not using sparse files, otherwise just make sure we have some room at least
		if ( _sparseFiles ) {
			ok = image_ensureDiskSpace( 2ull * 1024 * 1024 * 1024, false ); // 2GiB, maybe configurable one day
		} else {
			ok = image_ensureDiskSpace( remoteImageSize + ( 10 * 1024 * 1024 ), false ); // some extra space for cache map etc.
		}
		ok = ok && image_clone( sock, name, remoteRid, remoteImageSize ); // This sets up the file+map+crc and loads the img
		mutex_unlock( &reloadLock );
		if ( !ok ) goto server_fail;

		// Cloning worked :-)
		uplinkSock = sock;
		if ( !sock_sockaddrToDnbd3( (struct sockaddr*)&sa, &uplinkServer ) ) {
			uplinkServer.type = 0;
		}
		break;

server_fail: ;
		close( sock );
	}
	sock_destroyPollList( cons );

	// If we still have a pointer to a local image, release the reference
	if ( image != NULL ) image_release( image );
	// If everything worked out, this call should now actually return the image
	image = image_get( name, remoteRid, false );
	if ( image != NULL && uplinkSock != -1 ) {
		// If so, init the uplink and pass it the socket
		sock_setTimeout( uplinkSock, _uplinkTimeout );
		if ( !uplink_init( image, uplinkSock, &uplinkServer, remoteProtocolVersion ) ) {
			close( uplinkSock );
		} else {
			// Clumsy busy wait, but this should only take as long as it takes to start a thread, so is it really worth using a signalling mechanism?
			int i = 0;
			while ( !image->working && ++i < 100 )
				usleep( 2000 );
		}
	} else if ( uplinkSock != -1 ) {
		close( uplinkSock );
	}
	return image;
}

/**
 * Called if specific rid is not loaded, or if rid is 0, in which case we check on
 * disk which revision is latest.
 */
static dnbd3_image_t *loadImageServer(char * const name, const uint16_t requestedRid)
{
	char imageFile[PATHLEN] = "";
	uint16_t detectedRid = 0;

	if ( requestedRid != 0 ) {
		snprintf( imageFile, PATHLEN, "%s/%s.r%d", _basePath, name, (int)requestedRid );
		detectedRid = requestedRid;
	} else {
		glob_t g;
		snprintf( imageFile, PATHLEN, "%s/%s.r*", _basePath, name );
		const int ret = glob( imageFile, GLOB_NOSORT | GLOB_MARK, NULL, &g );
		imageFile[0] = '\0';
		if ( ret == 0 ) {
			long int best = 0;
			for ( size_t i = 0; i < g.gl_pathc; ++i ) {
				const char * const path = g.gl_pathv[i];
				const char * rev = strrchr( path, 'r' );
				if ( rev == NULL || rev == path || *(rev - 1) != '.' ) continue;
				rev++;
				if ( *rev < '0' || *rev > '9' ) continue;
				char *err = NULL;
				long int val = strtol( rev, &err, 10 );
				if ( err == NULL || *err != '\0' ) continue;
				if ( val > best ) {
					best = val;
					snprintf( imageFile, PATHLEN, "%s", g.gl_pathv[i] );
				}
			}
			if ( best > 0 && best < 65536 ) {
				detectedRid = (uint16_t)best;
			}
		}
		globfree( &g );
	}
	if ( _vmdkLegacyMode && requestedRid <= 1
			&& !isForbiddenExtension( name )
			&& ( detectedRid == 0 || !file_isReadable( imageFile ) ) ) {
		snprintf( imageFile, PATHLEN, "%s/%s", _basePath, name );
		detectedRid = 1;
	}
	logadd( LOG_DEBUG2, "Trying to load %s:%d ( -> %d) as %s", name, (int)requestedRid, (int)detectedRid, imageFile );
	// No file was determined, or it doesn't seem to exist/be readable
	if ( detectedRid == 0 ) {
		logadd( LOG_DEBUG2, "Not found, bailing out" );
		return image_get( name, requestedRid, true );
	}
	if ( !_vmdkLegacyMode && requestedRid == 0 ) {
		// rid 0 requested - check if detected rid is readable, decrease rid if not until we reach 0
		while ( detectedRid != 0 ) {
			dnbd3_image_t *image = image_get( name, detectedRid, true );
			if ( image != NULL ) {
				// globbed rid already loaded, return
				return image;
			}
			if ( file_isReadable( imageFile ) ) {
				// globbed rid is
				break;
			}
			logadd( LOG_DEBUG2, "%s: rid %d globbed but not readable, trying lower rid...", name, (int)detectedRid );
			detectedRid--;
			snprintf( imageFile, PATHLEN, "%s/%s.r%d", _basePath, name, requestedRid );
		}
	}

	// Now lock on the loading mutex, then check again if the image exists (we're multi-threaded)
	mutex_lock( &reloadLock );
	dnbd3_image_t* image = image_get( name, detectedRid, true );
	if ( image != NULL ) {
		// The image magically appeared in the meantime
		logadd( LOG_DEBUG2, "Magically appeared" );
		mutex_unlock( &reloadLock );
		return image;
	}
	// Still not loaded, let's try to do so
	logadd( LOG_DEBUG2, "Calling load" );
	image_load( _basePath, imageFile, false );
	mutex_unlock( &reloadLock );
	// If loading succeeded, this will return the image
	logadd( LOG_DEBUG2, "Calling get" );
	return image_get( name, requestedRid, true );
}

/**
 * Prepare a cloned image:
 * 1. Allocate empty image file and its cache map
 * 2. Use passed socket to request the crc32 list and save it to disk
 * 3. Load the image from disk
 * Returns: true on success, false otherwise
 */
static bool image_clone(int sock, char *name, uint16_t revision, uint64_t imageSize)
{
	// Allocate disk space and create cache map
	if ( !image_create( name, revision, imageSize ) ) return false;
	// CRC32
	const size_t len = strlen( _basePath ) + strlen( name ) + 20;
	char crcFile[len];
	snprintf( crcFile, len, "%s/%s.r%d.crc", _basePath, name, (int)revision );
	if ( !file_isReadable( crcFile ) ) {
		// Get crc32list from remote server
		size_t crc32len = IMGSIZE_TO_HASHBLOCKS(imageSize) * sizeof(uint32_t);
		uint32_t masterCrc;
		uint8_t *crc32list = malloc( crc32len );
		if ( !dnbd3_get_crc32( sock, &masterCrc, crc32list, &crc32len ) ) {
			free( crc32list );
			return false;
		}
		if ( crc32len != 0 ) {
			uint32_t lists_crc = crc32( 0, NULL, 0 );
			lists_crc = crc32( lists_crc, (uint8_t*)crc32list, crc32len );
			lists_crc = net_order_32( lists_crc );
			if ( lists_crc != masterCrc ) {
				logadd( LOG_WARNING, "OTF-Clone: Corrupted CRC-32 list. ignored. (%s)", name );
			} else {
				int fd = open( crcFile, O_WRONLY | O_CREAT, 0644 );
				write( fd, &masterCrc, sizeof(uint32_t) );
				write( fd, crc32list, crc32len );
				close( fd );
			}
		}
		free( crc32list );
	}
	// HACK: Chop of ".crc" to get the image file name
	crcFile[strlen( crcFile ) - 4] = '\0';
	return image_load( _basePath, crcFile, false );
}

/**
 * Generate the crc32 block list file for the given file.
 * This function wants a plain file name instead of a dnbd3_image_t,
 * as it can be used directly from the command line.
 */
bool image_generateCrcFile(char *image)
{
	int fdCrc = -1;
	uint32_t crc;
	char crcFile[strlen( image ) + 4 + 1];
	int fdImage = open( image, O_RDONLY );

	if ( fdImage == -1 ) {
		logadd( LOG_ERROR, "Could not open %s.", image );
		return false;
	}

	const int64_t fileLen = lseek( fdImage, 0, SEEK_END );
	if ( fileLen <= 0 ) {
		logadd( LOG_ERROR, "Error seeking to end, or file is empty." );
		goto cleanup_fail;
	}

	struct stat sst;
	sprintf( crcFile, "%s.crc", image );
	if ( stat( crcFile, &sst ) == 0 ) {
		logadd( LOG_ERROR, "CRC File for %s already exists! Delete it first if you want to regen.", image );
		goto cleanup_fail;
	}

	fdCrc = open( crcFile, O_RDWR | O_CREAT, 0644 );
	if ( fdCrc == -1 ) {
		logadd( LOG_ERROR, "Could not open CRC File %s for writing..", crcFile );
		goto cleanup_fail;
	}
	// CRC of all CRCs goes first. Don't know it yet, write 4 bytes dummy data.
	if ( write( fdCrc, crcFile, sizeof(crc) ) != sizeof(crc) ) {
		logadd( LOG_ERROR, "Write error" );
		goto cleanup_fail;
	}

	printf( "Generating CRC32" );
	fflush( stdout );
	const int blockCount = IMGSIZE_TO_HASHBLOCKS( fileLen );
	for ( int i = 0; i < blockCount; ++i ) {
		if ( !image_calcBlockCrc32( fdImage, i, fileLen, &crc ) ) {
			goto cleanup_fail;
		}
		if ( write( fdCrc, &crc, sizeof(crc) ) != sizeof(crc) ) {
			printf( "\nWrite error writing crc file: %d\n", errno );
			goto cleanup_fail;
		}
		putchar( '.' );
		fflush( stdout );
	}
	close( fdImage );
	fdImage = -1;
	printf( "done!\n" );

	logadd( LOG_INFO, "Generating master-crc..." );
	fflush( stdout );
	// File is written - read again to calc master crc
	if ( lseek( fdCrc, 4, SEEK_SET ) != 4 ) {
		logadd( LOG_ERROR, "Could not seek to beginning of crc list in file" );
		goto cleanup_fail;
	}
	char buffer[400];
	int blocksToGo = blockCount;
	crc = crc32( 0, NULL, 0 );
	while ( blocksToGo > 0 ) {
		const int numBlocks = MIN( (int)( sizeof(buffer) / sizeof(crc) ), blocksToGo );
		if ( read( fdCrc, buffer, numBlocks * sizeof(crc) ) != numBlocks * (int)sizeof(crc) ) {
			logadd( LOG_ERROR, "Could not re-read from crc32 file" );
			goto cleanup_fail;
		}
		crc = crc32( crc, (uint8_t*)buffer, numBlocks * sizeof(crc) );
		blocksToGo -= numBlocks;
	}
	crc = net_order_32( crc );
	if ( pwrite( fdCrc, &crc, sizeof(crc), 0 ) != sizeof(crc) ) {
		logadd( LOG_ERROR, "Could not write master crc to file" );
		goto cleanup_fail;
	}
	logadd( LOG_INFO, "CRC-32 file successfully generated." );
	fflush( stdout );
	return true;

cleanup_fail:;
	if ( fdImage != -1 ) close( fdImage );
	if ( fdCrc != -1 ) close( fdCrc );
	return false;
}

json_t* image_getListAsJson()
{
	json_t *imagesJson = json_array();
	json_t *jsonImage;
	int i;
	char uplinkName[100] = { 0 };
	uint64_t bytesReceived;
	int users, completeness, idleTime;
	declare_now;

	mutex_lock( &imageListLock );
	for ( i = 0; i < _num_images; ++i ) {
		if ( _images[i] == NULL ) continue;
		dnbd3_image_t *image = _images[i];
		mutex_lock( &image->lock );
		mutex_unlock( &imageListLock );
		users = image->users;
		idleTime = (int)timing_diff( &image->atime, &now );
		completeness = image_getCompletenessEstimate( image );
		if ( image->uplink == NULL ) {
			bytesReceived = 0;
			uplinkName[0] = '\0';
		} else {
			bytesReceived = image->uplink->bytesReceived;
			if ( image->uplink->fd == -1 || !host_to_string( &image->uplink->currentServer, uplinkName, sizeof(uplinkName) ) ) {
				uplinkName[0] = '\0';
			}
		}
		image->users++; // Prevent freeing after we unlock
		mutex_unlock( &image->lock );

		jsonImage = json_pack( "{sisssisisisisI}",
				"id", image->id, // id, name, rid never change, so access them without locking
				"name", image->name,
				"rid", (int) image->rid,
				"users", users,
				"complete",  completeness,
				"idle", idleTime,
				"size", (json_int_t)image->virtualFilesize );
		if ( bytesReceived != 0 ) {
			json_object_set_new( jsonImage, "bytesReceived", json_integer( (json_int_t) bytesReceived ) );
		}
		if ( uplinkName[0] != '\0' ) {
			json_object_set_new( jsonImage, "uplinkServer", json_string( uplinkName ) );
		}
		json_array_append_new( imagesJson, jsonImage );

		image = image_release( image ); // Since we did image->users++;
		mutex_lock( &imageListLock );
	}
	mutex_unlock( &imageListLock );
	return imagesJson;
}

/**
 * Get completeness of an image in percent. Only estimated, not exact.
 * Returns: 0-100
 * DOES NOT LOCK, so make sure to do so before calling
 */
int image_getCompletenessEstimate(dnbd3_image_t * const image)
{
	assert( image != NULL );
	if ( image->cache_map == NULL ) return image->working ? 100 : 0;
	declare_now;
	if ( !timing_reached( &image->nextCompletenessEstimate, &now ) ) {
		// Since this operation is relatively expensive, we cache the result for a while
		return image->completenessEstimate;
	}
	int i;
	int percent = 0;
	const int len = IMGSIZE_TO_MAPBYTES( image->virtualFilesize );
	if ( len == 0 ) return 0;
	for ( i = 0; i < len; ++i ) {
		if ( image->cache_map[i] == 0xff ) {
			percent += 100;
		} else if ( image->cache_map[i] != 0 ) {
			percent += 50;
		}
	}
	image->completenessEstimate = percent / len;
	timing_set( &image->nextCompletenessEstimate, &now, 8 + rand() % 32 );
	return image->completenessEstimate;
}

/**
 * Check the CRC-32 of the given blocks. The array "blocks" is of variable length.
 * !! pass -1 as the last block so the function knows when to stop !!
 * Does NOT check whether block index is within image.
 * Returns true or false
 */
bool image_checkBlocksCrc32(const int fd, uint32_t *crc32list, const int *blocks, const uint64_t realFilesize)
{
	while ( *blocks != -1 ) {
		uint32_t crc;
		if ( !image_calcBlockCrc32( fd, *blocks, realFilesize, &crc ) ) {
			return false;
		}
		if ( crc != crc32list[*blocks] ) {
			logadd( LOG_WARNING, "Block %d is %x, should be %x", *blocks, crc, crc32list[*blocks] );
			return false;
		}
		blocks++;
	}
	return true;
}

/**
 * Calc CRC-32 of block. Value is returned as little endian.
 */
static bool image_calcBlockCrc32(const int fd, const size_t block, const uint64_t realFilesize, uint32_t *crc)
{
	// Make buffer 4k aligned in case fd has O_DIRECT set
#define BSIZE 262144
	char rawBuffer[BSIZE + DNBD3_BLOCK_SIZE];
	char * const buffer = (char*)( ( (uintptr_t)rawBuffer + ( DNBD3_BLOCK_SIZE - 1 ) ) & ~( DNBD3_BLOCK_SIZE - 1 ) );
	// How many bytes to read from the input file
	const uint64_t bytesFromFile = MIN( HASH_BLOCK_SIZE, realFilesize - ( block * HASH_BLOCK_SIZE) );
	// Determine how many bytes we had to read if the file size were a multiple of 4k
	// This might be the same value if the real file's size is a multiple of 4k
	const uint64_t vbs = ( ( realFilesize + ( DNBD3_BLOCK_SIZE - 1 ) ) & ~( DNBD3_BLOCK_SIZE - 1 ) ) - ( block * HASH_BLOCK_SIZE );
	const uint64_t virtualBytesFromFile = MIN( HASH_BLOCK_SIZE, vbs );
	const off_t readPos = (int64_t)block * HASH_BLOCK_SIZE;
	size_t bytes = 0;
	assert( vbs >= bytesFromFile );
	*crc = crc32( 0, NULL, 0 );
	// Calculate the crc32 by reading data from the file
	while ( bytes < bytesFromFile ) {
		const size_t n = (size_t)MIN( BSIZE, bytesFromFile - bytes );
		const ssize_t r = pread( fd, buffer, n, readPos + bytes );
		if ( r <= 0 ) {
			logadd( LOG_WARNING, "CRC: Read error (errno=%d)", errno );
			return false;
		}
		*crc = crc32( *crc, (uint8_t*)buffer, r );
		bytes += (size_t)r;
	}
	// If the virtual file size is different, keep going using nullbytes
	if ( bytesFromFile < virtualBytesFromFile ) {
		memset( buffer, 0, BSIZE );
		bytes = (size_t)( virtualBytesFromFile - bytesFromFile );
		while ( bytes != 0 ) {
			const size_t len = MIN( BSIZE, bytes );
			*crc = crc32( *crc, (uint8_t*)buffer, len );
			bytes -= len;
		}
	}
	*crc = net_order_32( *crc );
	return true;
#undef BSIZE
}

/**
 * Call image_ensureDiskSpace (below), but aquire
 * reloadLock first.
 */
bool image_ensureDiskSpaceLocked(uint64_t size, bool force)
{
	bool ret;
	mutex_lock( &reloadLock );
	ret = image_ensureDiskSpace( size, force );
	mutex_unlock( &reloadLock );
	return ret;
}

/**
 * Make sure at least size bytes are available in _basePath.
 * Will delete old images to make room for new ones.
 * TODO: Store last access time of images. Currently the
 * last access time is reset to the file modification time
 * on server restart. Thus it will
 * currently only delete images if server uptime is > 10 hours.
 * This can be overridden by setting force to true, in case
 * free space is desperately needed.
 * Return true iff enough space is available. false in random other cases
 */
static bool image_ensureDiskSpace(uint64_t size, bool force)
{
	for ( int maxtries = 0; maxtries < 20; ++maxtries ) {
		uint64_t available;
		if ( !file_freeDiskSpace( _basePath, NULL, &available ) ) {
			const int e = errno;
			logadd( LOG_WARNING, "Could not get free disk space (errno %d), will assume there is enough space left... ;-)\n", e );
			return true;
		}
		if ( available > size ) return true;
		if ( !force && dnbd3_serverUptime() < 10 * 3600 ) {
			logadd( LOG_INFO, "Only %dMiB free, %dMiB requested, but server uptime < 10 hours...", (int)(available / (1024ll * 1024ll)),
					(int)(size / (1024 * 1024)) );
			return false;
		}
		logadd( LOG_INFO, "Only %dMiB free, %dMiB requested, freeing an image...", (int)(available / (1024ll * 1024ll)),
				(int)(size / (1024 * 1024)) );
		// Find least recently used image
		dnbd3_image_t *oldest = NULL;
		int i; // XXX improve locking
		for (i = 0; i < _num_images; ++i) {
			if ( _images[i] == NULL ) continue;
			dnbd3_image_t *current = image_lock( _images[i] );
			if ( current == NULL ) continue;
			if ( current->users == 1 ) { // Just from the lock above
				if ( oldest == NULL || timing_1le2( &current->atime, &oldest->atime ) ) {
					// Oldest access time so far
					oldest = current;
				}
			}
			current = image_release( current );
		}
		declare_now;
		if ( oldest == NULL || ( !_sparseFiles && timing_diff( &oldest->atime, &now ) < 86400 ) ) {
			if ( oldest == NULL ) {
				logadd( LOG_INFO, "All images are currently in use :-(" );
			} else {
				logadd( LOG_INFO, "Won't free any image, all have been in use in the past 24 hours :-(" );
			}
			return false;
		}
		oldest = image_lock( oldest );
		if ( oldest == NULL ) continue; // Image freed in the meantime? Try again
		logadd( LOG_INFO, "'%s:%d' has to go!", oldest->name, (int)oldest->rid );
		char *filename = strdup( oldest->path );
		oldest = image_remove( oldest );
		oldest = image_release( oldest );
		unlink( filename );
		size_t len = strlen( filename ) + 10;
		char buffer[len];
		snprintf( buffer, len, "%s.map", filename );
		unlink( buffer );
		snprintf( buffer, len, "%s.crc", filename );
		unlink( buffer );
		snprintf( buffer, len, "%s.meta", filename );
		unlink( buffer );
		free( filename );
	}
	return false;
}

void image_closeUnusedFd()
{
	int fd, i;
	ticks deadline;
	timing_gets( &deadline, -UNUSED_FD_TIMEOUT );
	char imgstr[300];
	mutex_lock( &imageListLock );
	for (i = 0; i < _num_images; ++i) {
		dnbd3_image_t * const image = _images[i];
		if ( image == NULL )
			continue;
		mutex_lock( &image->lock );
		mutex_unlock( &imageListLock );
		if ( image->users == 0 && image->uplink == NULL && timing_reached( &image->atime, &deadline ) ) {
			snprintf( imgstr, sizeof(imgstr), "%s:%d", image->name, (int)image->rid );
			fd = image->readFd;
			image->readFd = -1;
		} else {
			fd = -1;
		}
		mutex_unlock( &image->lock );
		if ( fd != -1 ) {
			close( fd );
			logadd( LOG_DEBUG1, "Inactive fd closed for %s", imgstr );
		}
		mutex_lock( &imageListLock );
	}
	mutex_unlock( &imageListLock );
}

/*
 void image_find_latest()
 {
 // Not in array or most recent rid is requested, try file system
 if (revision != 0) {
 // Easy case - specific RID
 char
 } else {
 // Determine base directory where the image in question has to reside.
 // Eg, the _basePath is "/srv/", requested image is "rz/ubuntu/default-13.04"
 // Then searchPath has to be set to "/srv/rz/ubuntu"
 char searchPath[strlen(_basePath) + len + 1];
 char *lastSlash = strrchr(name, '/');
 char *baseName; // Name of the image. In the example above, it will be "default-13.04"
 if ( lastSlash == NULL ) {
 *searchPath = '\0';
 baseName = name;
 } else {
 char *from = name, *to = searchPath;
 while (from < lastSlash) *to++ = *from++;
 *to = '\0';
 baseName = lastSlash + 1;
 }
 // Now we have the search path in our real file system and the expected image name.
 // The revision naming sceme is <IMAGENAME>.r<RID>, so if we're looking for revision 13,
 // our example image has to be named default-13.04.r13
 }
 }
 */
