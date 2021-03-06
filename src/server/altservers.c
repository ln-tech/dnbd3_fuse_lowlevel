#include "altservers.h"
#include "locks.h"
#include "helper.h"
#include "image.h"
#include "fileutil.h"
#include "../shared/protocol.h"
#include "../shared/timing.h"
#include "../serverconfig.h"
#include <assert.h>
#include <inttypes.h>
#include <jansson.h>

#define LOG(lvl, msg, ...) logadd(lvl, msg " (%s:%d)", __VA_ARGS__, image->name, (int)image->rid)
#define LOG_GOTO(jumplabel, lvl, ...) do { LOG(lvl, __VA_ARGS__); goto jumplabel; } while (0);
#define ERROR_GOTO(jumplabel, ...) LOG_GOTO(jumplabel, LOG_ERROR, __VA_ARGS__)

static dnbd3_connection_t *pending[SERVER_MAX_PENDING_ALT_CHECKS];
static pthread_mutex_t pendingLockWrite; // Lock for adding something to pending. (NULL -> nonNULL)
static pthread_mutex_t pendingLockConsume; // Lock for removing something (nonNULL -> NULL)
static dnbd3_signal_t* runSignal = NULL;

static dnbd3_alt_server_t altServers[SERVER_MAX_ALTS];
static int numAltServers = 0;
static pthread_mutex_t altServersLock;

static pthread_t altThread;

static void *altservers_main(void *data);
static unsigned int altservers_updateRtt(const dnbd3_host_t * const host, const unsigned int rtt);

void altservers_init()
{
	srand( (unsigned int)time( NULL ) );
	// Init spinlock
	mutex_init( &pendingLockWrite );
	mutex_init( &pendingLockConsume );
	mutex_init( &altServersLock );
	// Init signal
	runSignal = signal_new();
	if ( runSignal == NULL ) {
		logadd( LOG_ERROR, "Error creating signal object. Uplink feature unavailable." );
		exit( EXIT_FAILURE );
	}
	memset( altServers, 0, SERVER_MAX_ALTS * sizeof(dnbd3_alt_server_t) );
	if ( 0 != thread_create( &altThread, NULL, &altservers_main, (void *)NULL ) ) {
		logadd( LOG_ERROR, "Could not start altservers connector thread" );
		exit( EXIT_FAILURE );
	}
	// Init waiting links queue -- this is currently a global static array so
	// it will already be zero, but in case we refactor later do it explicitly
	// while also holding the write lock so thread sanitizer is happy
	mutex_lock( &pendingLockWrite );
	for (int i = 0; i < SERVER_MAX_PENDING_ALT_CHECKS; ++i) {
		pending[i] = NULL;
	}
	mutex_unlock( &pendingLockWrite );
}

void altservers_shutdown()
{
	if ( runSignal == NULL ) return;
	signal_call( runSignal ); // Wake altservers thread up
	thread_join( altThread, NULL );
}

static void addalt(int argc, char **argv, void *data)
{
	char *shost;
	dnbd3_host_t host;
	bool isPrivate = false;
	bool isClientOnly = false;
	if ( argv[0][0] == '#' ) return;
	for (shost = argv[0]; *shost != '\0'; ) { // Trim left and scan for "-" prefix
		if ( *shost == '-' ) isPrivate = true;
		else if ( *shost == '+' ) isClientOnly = true;
		else if ( *shost != ' ' && *shost != '\t' ) break;
		shost++;
	}
	if ( !parse_address( shost, &host ) ) {
		logadd( LOG_WARNING, "Invalid entry in alt-servers file ignored: '%s'", shost );
		return;
	}
	if ( argc == 1 ) argv[1] = "";
	if ( altservers_add( &host, argv[1], isPrivate, isClientOnly ) ) {
		(*(int*)data)++;
	}
}

int altservers_load()
{
	int count = 0;
	char *name;
	if ( asprintf( &name, "%s/%s", _configDir, "alt-servers" ) == -1 ) return -1;
	file_loadLineBased( name, 1, 2, &addalt, (void*)&count );
	free( name );
	logadd( LOG_DEBUG1, "Added %d alt servers\n", count );
	return count;
}

bool altservers_add(dnbd3_host_t *host, const char *comment, const int isPrivate, const int isClientOnly)
{
	int i, freeSlot = -1;
	mutex_lock( &altServersLock );
	for (i = 0; i < numAltServers; ++i) {
		if ( isSameAddressPort( &altServers[i].host, host ) ) {
			mutex_unlock( &altServersLock );
			return false;
		} else if ( freeSlot == -1 && altServers[i].host.type == 0 ) {
			freeSlot = i;
		}
	}
	if ( freeSlot == -1 ) {
		if ( numAltServers >= SERVER_MAX_ALTS ) {
			logadd( LOG_WARNING, "Cannot add another alt server, maximum of %d already reached.", (int)SERVER_MAX_ALTS );
			mutex_unlock( &altServersLock );
			return false;
		}
		freeSlot = numAltServers++;
	}
	altServers[freeSlot].host = *host;
	altServers[freeSlot].isPrivate = isPrivate;
	altServers[freeSlot].isClientOnly = isClientOnly;
	if ( comment != NULL ) snprintf( altServers[freeSlot].comment, COMMENT_LENGTH, "%s", comment );
	mutex_unlock( &altServersLock );
	return true;
}

/**
 * ONLY called from the passed uplink's main thread
 */
void altservers_findUplink(dnbd3_connection_t *uplink)
{
	int i;
	// if betterFd != -1 it means the uplink is supposed to switch to another
	// server. As this function here is called by the uplink thread, it can
	// never be that the uplink is supposed to switch, but instead calls
	// this function.
	assert( uplink->betterFd == -1 );
	mutex_lock( &pendingLockWrite );
	// it is however possible that an RTT measurement is currently in progress,
	// so check for that case and do nothing if one is in progress
	if ( uplink->rttTestResult == RTT_INPROGRESS ) {
		for (i = 0; i < SERVER_MAX_PENDING_ALT_CHECKS; ++i) {
			if ( pending[i] != uplink ) continue;
			// Yep, measuring right now
			mutex_unlock( &pendingLockWrite );
			return;
		}
	}
	// Find free slot for measurement
	for (i = 0; i < SERVER_MAX_PENDING_ALT_CHECKS; ++i) {
		if ( pending[i] != NULL ) continue;
		pending[i] = uplink;
		uplink->rttTestResult = RTT_INPROGRESS;
		mutex_unlock( &pendingLockWrite );
		signal_call( runSignal ); // Wake altservers thread up
		return;
	}
	// End of loop - no free slot
	mutex_unlock( &pendingLockWrite );
	logadd( LOG_WARNING, "No more free RTT measurement slots, ignoring a request..." );
}

/**
 * The given uplink is about to disappear, so remove it from any queues
 */
void altservers_removeUplink(dnbd3_connection_t *uplink)
{
	mutex_lock( &pendingLockConsume );
	mutex_lock( &pendingLockWrite );
	for (int i = 0; i < SERVER_MAX_PENDING_ALT_CHECKS; ++i) {
		if ( pending[i] == uplink ) {
			uplink->rttTestResult = RTT_NOT_REACHABLE;
			pending[i] = NULL;
		}
	}
	mutex_unlock( &pendingLockWrite );
	mutex_unlock( &pendingLockConsume );
}

/**
 * Get <size> known (working) alt servers, ordered by network closeness
 * (by finding the smallest possible subnet)
 * Private servers are excluded, so this is what you want to call to
 * get a list of servers you can tell a client about
 */
int altservers_getListForClient(dnbd3_host_t *host, dnbd3_server_entry_t *output, int size)
{
	if ( host == NULL || host->type == 0 || numAltServers == 0 || output == NULL || size <= 0 ) return 0;
	int i, j;
	int count = 0;
	int scores[size];
	int score;
	mutex_lock( &altServersLock );
	if ( size > numAltServers ) size = numAltServers;
	for (i = 0; i < numAltServers; ++i) {
		if ( altServers[i].host.type == 0 ) continue; // Slot is empty
		if ( altServers[i].isPrivate ) continue; // Do not tell clients about private servers
		if ( host->type == altServers[i].host.type ) {
			score = altservers_netCloseness( host, &altServers[i].host ) - altServers[i].numFails;
		} else {
			score = -( altServers[i].numFails + 128 ); // Wrong address family
		}
		if ( count == 0 ) {
			// Trivial - this is the first entry
			output[0].host = altServers[i].host;
			output[0].failures = 0;
			scores[0] = score;
			count++;
		} else {
			// Other entries already exist, insert in proper position
			for (j = 0; j < size; ++j) {
				if ( j < count && score <= scores[j] ) continue;
				if ( j > count ) break; // Should never happen but just in case...
				if ( j < count && j + 1 < size ) {
					// Check if we're in the middle and need to move other entries...
					memmove( &output[j + 1], &output[j], sizeof(dnbd3_server_entry_t) * (size - j - 1) );
					memmove( &scores[j + 1], &scores[j], sizeof(int) * (size - j - 1) );
				}
				if ( count < size ) {
					count++;
				}
				output[j].host = altServers[i].host;
				output[j].failures = 0;
				scores[j] = score;
				break;
			}
		}
	}
	mutex_unlock( &altServersLock );
	return count;
}

/**
 * Get <size> alt servers. If there are more alt servers than
 * requested, random servers will be picked.
 * This function is suited for finding uplink servers as
 * it includes private servers and ignores any "client only" servers
 */
int altservers_getListForUplink(dnbd3_host_t *output, int size, int emergency)
{
	if ( size <= 0 ) return 0;
	int count = 0, i;
	ticks now;
	timing_get( &now );
	mutex_lock( &altServersLock );
	// Flip first server in list with a random one every time this is called
	if ( numAltServers > 1 ) {
		const dnbd3_alt_server_t tmp = altServers[0];
		do {
			i = rand() % numAltServers;
		} while ( i == 0 );
		altServers[0] = altServers[i];
		altServers[i] = tmp;
	}
	// We iterate over the list twice. First run adds servers with 0 failures only,
	// second one also considers those that failed (not too many times)
	if ( size > numAltServers ) size = numAltServers;
	for (i = 0; i < numAltServers * 2; ++i) {
		dnbd3_alt_server_t *srv = &altServers[i % numAltServers];
		if ( srv->host.type == 0 ) continue; // Slot is empty
		if ( _proxyPrivateOnly && !srv->isPrivate ) continue; // Config says to consider private alt-servers only? ignore!
		if ( srv->isClientOnly ) continue;
		bool first = ( i < numAltServers );
		if ( first ) {
			if ( srv->numFails > 0 ) continue;
		} else {
			if ( srv->numFails == 0 ) continue; // Already added in first iteration
			if ( !emergency && srv->numFails > SERVER_BAD_UPLINK_THRES // server failed X times in a row
				&& timing_diff( &srv->lastFail, &now ) < SERVER_BAD_UPLINK_IGNORE ) continue; // and last fail was not too long ago? ignore!
			if ( !emergency ) srv->numFails--;
		}
		// server seems ok, include in output and decrease its fail counter
		output[count++] = srv->host;
		if ( count >= size ) break;
	}
	mutex_unlock( &altServersLock );
	return count;
}

json_t* altservers_toJson()
{
	json_t *list = json_array();

	mutex_lock( &altServersLock );
	char host[100];
	const int count = numAltServers;
	dnbd3_alt_server_t src[count];
	memcpy( src, altServers, sizeof(src) );
	mutex_unlock( &altServersLock );
	for (int i = 0; i < count; ++i) {
		json_t *rtts = json_array();
		for (int j = 0; j < SERVER_RTT_PROBES; ++j) {
			json_array_append_new( rtts, json_integer( src[i].rtt[ (j + src[i].rttIndex + 1) % SERVER_RTT_PROBES ] ) );
		}
		sock_printHost( &src[i].host, host, sizeof(host) );
		json_t *server = json_pack( "{ss,ss,so,sb,sb,si}",
			"comment", src[i].comment,
			"host", host,
			"rtt", rtts,
			"isPrivate", (int)src[i].isPrivate,
			"isClientOnly", (int)src[i].isClientOnly,
			"numFails", src[i].numFails
		);
		json_array_append_new( list, server );
	}
	return list;
}

/**
 * Update rtt history of given server - returns the new average for that server
 */
static unsigned int altservers_updateRtt(const dnbd3_host_t * const host, const unsigned int rtt)
{
	unsigned int avg = rtt;
	int i;
	mutex_lock( &altServersLock );
	for (i = 0; i < numAltServers; ++i) {
		if ( !isSameAddressPort( host, &altServers[i].host ) ) continue;
		altServers[i].rtt[++altServers[i].rttIndex % SERVER_RTT_PROBES] = rtt;
#if SERVER_RTT_PROBES == 5
		avg = (altServers[i].rtt[0] + altServers[i].rtt[1] + altServers[i].rtt[2]
				+ altServers[i].rtt[3] + altServers[i].rtt[4]) / SERVER_RTT_PROBES;
#else
#warning You might want to change the code in altservers_update_rtt if you changed SERVER_RTT_PROBES
		avg = 0;
		for (int j = 0; j < SERVER_RTT_PROBES; ++j) {
			avg += altServers[i].rtt[j];
		}
		avg /= SERVER_RTT_PROBES;
#endif
		// If we got a new rtt value, server must be working
		if ( altServers[i].numFails > 0 ) {
			altServers[i].numFails--;
		}
		break;
	}
	mutex_unlock( &altServersLock );
	return avg;
}

/**
 * Determine how close two addresses are to each other by comparing the number of
 * matching bits from the left of the address. Does not count individual bits but
 * groups of 4 for speed.
 * Return: Closeness - higher number means closer
 */
int altservers_netCloseness(dnbd3_host_t *host1, dnbd3_host_t *host2)
{
	if ( host1 == NULL || host2 == NULL || host1->type != host2->type ) return -1;
	int retval = 0;
	const int max = host1->type == HOST_IP4 ? 4 : 16;
	for (int i = 0; i < max; ++i) {
		if ( (host1->addr[i] & 0xf0) != (host2->addr[i] & 0xf0) ) return retval;
		++retval;
		if ( (host1->addr[i] & 0x0f) != (host2->addr[i] & 0x0f) ) return retval;
		++retval;
	}
	return retval;
}

/**
 * Called if an uplink server failed during normal uplink operation. This unit keeps
 * track of how often servers fail, and consider them disabled for some time if they
 * fail too many times.
 */
void altservers_serverFailed(const dnbd3_host_t * const host)
{
	int i;
	int foundIndex = -1, lastOk = -1;
	ticks now;
	timing_get( &now );
	mutex_lock( &altServersLock );
	for (i = 0; i < numAltServers; ++i) {
		if ( foundIndex == -1 ) {
			// Looking for the failed server in list
			if ( isSameAddressPort( host, &altServers[i].host ) ) {
				foundIndex = i;
			}
		} else if ( altServers[i].host.type != 0 && altServers[i].numFails == 0 ) {
			lastOk = i;
		}
	}
	// Do only increase counter if last fail was not too recent. This is
	// to prevent the counter from increasing rapidly if many images use the
	// same uplink. If there's a network hickup, all uplinks will call this
	// function and would increase the counter too quickly, disabling the server.
	if ( foundIndex != -1 && timing_diff( &altServers[foundIndex].lastFail, &now ) > SERVER_RTT_INTERVAL_INIT ) {
		altServers[foundIndex].numFails += SERVER_UPLINK_FAIL_INCREASE;
		altServers[foundIndex].lastFail = now;
		if ( lastOk != -1 ) {
			// Make sure non-working servers are put at the end of the list, so they're less likely
			// to get picked when testing servers for uplink connections.
			const dnbd3_alt_server_t tmp = altServers[foundIndex];
			altServers[foundIndex] = altServers[lastOk];
			altServers[lastOk] = tmp;
		}
	}
	mutex_unlock( &altServersLock );
}
/**
 * Mainloop of this module. It will wait for requests by uplinks to find a
 * suitable uplink server for them. If found, it will tell the uplink about
 * the best server found. Currently the RTT history is kept per server and
 * not per uplink, so if many images use the same uplink server, the history
 * will update quite quickly. Needs to be improved some time, ie. by only
 * updating the rtt if the last update was at least X seconds ago.
 */
static void *altservers_main(void *data UNUSED)
{
	const int ALTS = 4;
	int ret, itLink, itAlt, numAlts;
	bool found;
	char buffer[DNBD3_BLOCK_SIZE ];
	dnbd3_reply_t reply;
	dnbd3_host_t servers[ALTS + 1];
	serialized_buffer_t serialized;
	struct timespec start, end;
	ticks nextCloseUnusedFd;

	setThreadName( "altserver-check" );
	blockNoncriticalSignals();
	timing_gets( &nextCloseUnusedFd, 900 );
	// LOOP
	while ( !_shutdown ) {
		// Wait 5 seconds max.
		ret = signal_wait( runSignal, 5000 );
		if ( _shutdown ) goto cleanup;
		if ( ret == SIGNAL_ERROR ) {
			if ( errno == EAGAIN || errno == EINTR ) continue;
			logadd( LOG_WARNING, "Error %d on signal_clear on alservers_main! Things will break!", errno );
			usleep( 100000 );
		}
		// Work your way through the queue
		for (itLink = 0; itLink < SERVER_MAX_PENDING_ALT_CHECKS; ++itLink) {
			mutex_lock( &pendingLockWrite );
			if ( pending[itLink] == NULL ) {
				mutex_unlock( &pendingLockWrite );
				continue; // Check once before locking, as a mutex is expensive
			}
			mutex_unlock( &pendingLockWrite );
			mutex_lock( &pendingLockConsume );
			mutex_lock( &pendingLockWrite );
			dnbd3_connection_t * const uplink = pending[itLink];
			mutex_unlock( &pendingLockWrite );
			if ( uplink == NULL ) { // Check again after locking
				mutex_unlock( &pendingLockConsume );
				continue;
			}
			dnbd3_image_t * const image = image_lock( uplink->image );
			if ( image == NULL ) { // Check again after locking
				uplink->rttTestResult = RTT_NOT_REACHABLE;
				mutex_lock( &pendingLockWrite );
				pending[itLink] = NULL;
				mutex_unlock( &pendingLockWrite );
				mutex_unlock( &pendingLockConsume );
				logadd( LOG_DEBUG1, "Image has gone away that was queued for RTT measurement" );
				continue;
			}
			LOG( LOG_DEBUG2, "[%d] Running alt check", itLink );
			assert( uplink->rttTestResult == RTT_INPROGRESS );
			// Now get 4 alt servers
			numAlts = altservers_getListForUplink( servers, ALTS, uplink->fd == -1 );
			if ( uplink->fd != -1 ) {
				// Add current server if not already in list
				found = false;
				for (itAlt = 0; itAlt < numAlts; ++itAlt) {
					if ( !isSameAddressPort( &uplink->currentServer, &servers[itAlt] ) ) continue;
					found = true;
					break;
				}
				if ( !found ) servers[numAlts++] = uplink->currentServer;
			}
			// Test them all
			int bestSock = -1;
			int bestIndex = -1;
			int bestProtocolVersion = -1;
			unsigned long bestRtt = RTT_UNREACHABLE;
			unsigned long currentRtt = RTT_UNREACHABLE;
			for (itAlt = 0; itAlt < numAlts; ++itAlt) {
				usleep( 1000 ); // Wait a very short moment for the network to recover (we might be doing lots of measurements...)
				// Connect
				clock_gettime( BEST_CLOCK_SOURCE, &start );
				int sock = sock_connect( &servers[itAlt], 750, 1000 );
				if ( sock < 0 ) continue;
				// Select image ++++++++++++++++++++++++++++++
				if ( !dnbd3_select_image( sock, image->name, image->rid, SI_SERVER_FLAGS ) ) {
					goto server_failed;
				}
				// See if selecting the image succeeded ++++++++++++++++++++++++++++++
				uint16_t protocolVersion, rid;
				uint64_t imageSize;
				char *name;
				if ( !dnbd3_select_image_reply( &serialized, sock, &protocolVersion, &name, &rid, &imageSize ) ) {
					goto server_image_not_available;
				}
				if ( protocolVersion < MIN_SUPPORTED_SERVER ) goto server_failed;
				if ( name == NULL || strcmp( name, image->name ) != 0 ) {
					ERROR_GOTO( server_failed, "[RTT] Server offers image '%s'", name );
				}
				if ( rid != image->rid ) {
					ERROR_GOTO( server_failed, "[RTT] Server provides rid %d", (int)rid );
				}
				if ( imageSize != image->virtualFilesize ) {
					ERROR_GOTO( server_failed, "[RTT] Remote size: %" PRIu64 ", expected: %" PRIu64, imageSize, image->virtualFilesize );
				}
				// Request first block (NOT random!) ++++++++++++++++++++++++++++++
				if ( !dnbd3_get_block( sock, 0, DNBD3_BLOCK_SIZE, 0, COND_HOPCOUNT( protocolVersion, 1 ) ) ) {
					LOG_GOTO( server_failed, LOG_DEBUG1, "[RTT%d] Could not request first block", itLink );
				}
				// See if requesting the block succeeded ++++++++++++++++++++++
				if ( !dnbd3_get_reply( sock, &reply ) ) {
					LOG_GOTO( server_failed, LOG_DEBUG1, "[RTT%d] Received corrupted reply header after CMD_GET_BLOCK", itLink );
				}
				// check reply header
				if ( reply.cmd != CMD_GET_BLOCK || reply.size != DNBD3_BLOCK_SIZE ) {
					ERROR_GOTO( server_failed, "[RTT] Reply to first block request is %" PRIu32 " bytes", reply.size );
				}
				if ( recv( sock, buffer, DNBD3_BLOCK_SIZE, MSG_WAITALL ) != DNBD3_BLOCK_SIZE ) {
					ERROR_GOTO( server_failed, "[RTT%d] Could not read first block payload", itLink );
				}
				clock_gettime( BEST_CLOCK_SOURCE, &end );
				// Measurement done - everything fine so far
				mutex_lock( &uplink->rttLock );
				const bool isCurrent = isSameAddressPort( &servers[itAlt], &uplink->currentServer );
				// Penaltize rtt if this was a cycle; this will treat this server with lower priority
				// in the near future too, so we prevent alternating between two servers that are both
				// part of a cycle and have the lowest latency.
				const unsigned int rtt = (unsigned int)((end.tv_sec - start.tv_sec) * 1000000
						+ (end.tv_nsec - start.tv_nsec) / 1000
						+ ( (isCurrent && uplink->cycleDetected) ? 1000000 : 0 )); // µs
				unsigned int avg = altservers_updateRtt( &servers[itAlt], rtt );
				// If a cycle was detected, or we lost connection to the current (last) server, penaltize it one time
				if ( ( uplink->cycleDetected || uplink->fd == -1 ) && isCurrent ) avg = (avg * 2) + 50000;
				mutex_unlock( &uplink->rttLock );
				if ( uplink->fd != -1 && isCurrent ) {
					// Was measuring current server
					currentRtt = avg;
					close( sock );
				} else if ( avg < bestRtt ) {
					// Was another server, update "best"
					if ( bestSock != -1 ) close( bestSock );
					bestSock = sock;
					bestRtt = avg;
					bestIndex = itAlt;
					bestProtocolVersion = protocolVersion;
				} else {
					// Was too slow, ignore
					close( sock );
				}
				// We're done, call continue
				continue;
				// Jump here if anything went wrong
				// This will cleanup and continue
				server_failed: ;
				altservers_serverFailed( &servers[itAlt] );
				server_image_not_available: ;
				close( sock );
			}
			// Done testing all servers. See if we should switch
			if ( bestSock != -1 && (uplink->fd == -1 || (bestRtt < 10000000 && RTT_THRESHOLD_FACTOR(currentRtt) > bestRtt)) ) {
				// yep
				if ( currentRtt > 10000000 || uplink->fd == -1 ) {
					LOG( LOG_DEBUG1, "Change - best: %luµs, current: -", bestRtt );
				} else {
					LOG( LOG_DEBUG1, "Change - best: %luµs, current: %luµs", bestRtt, currentRtt );
				}
				sock_setTimeout( bestSock, _uplinkTimeout );
				mutex_lock( &uplink->rttLock );
				uplink->betterFd = bestSock;
				uplink->betterServer = servers[bestIndex];
				uplink->betterVersion = bestProtocolVersion;
				uplink->rttTestResult = RTT_DOCHANGE;
				mutex_unlock( &uplink->rttLock );
				signal_call( uplink->signal );
			} else if ( bestSock == -1 && currentRtt == RTT_UNREACHABLE ) {
				// No server was reachable
				mutex_lock( &uplink->rttLock );
				uplink->rttTestResult = RTT_NOT_REACHABLE;
				mutex_unlock( &uplink->rttLock );
			} else {
				// nope
				if ( bestSock != -1 ) close( bestSock );
				mutex_lock( &uplink->rttLock );
				uplink->rttTestResult = RTT_DONTCHANGE;
				uplink->cycleDetected = false; // It's a lie, but prevents rtt measurement triggering again right away
				mutex_unlock( &uplink->rttLock );
				if ( !image->working ) {
					image->working = true;
					LOG( LOG_DEBUG1, "[%d] No better alt server found, enabling again", itLink );
				}
			}
			image_release( image );
			// end of loop over all pending uplinks
			mutex_lock( &pendingLockWrite );
			pending[itLink] = NULL;
			mutex_unlock( &pendingLockWrite );
			mutex_unlock( &pendingLockConsume );
		}
		// Save cache maps of all images if applicable
		declare_now;
		// TODO: Has nothing to do with alt servers really, maybe move somewhere else?
		if ( _closeUnusedFd && timing_reached( &nextCloseUnusedFd, &now ) ) {
			timing_gets( &nextCloseUnusedFd, 900 );
			image_closeUnusedFd();
		}
	}
	cleanup: ;
	if ( runSignal != NULL ) signal_close( runSignal );
	runSignal = NULL;
	return NULL ;
}

