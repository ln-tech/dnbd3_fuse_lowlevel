#include "connection.h"
#include "helper.h"
#include "../clientconfig.h"
#include "../shared/protocol.h"
#include "../shared/fdsignal.h"
#include "../shared/sockhelper.h"
#include "../shared/log.h"

#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include "main.h"

/* Constants */
static const size_t SHORTBUF = 100;
#define MAX_ALTS (16)
#define MAX_ALTS_ACTIVE (5)
#define MAX_HOSTS_PER_ADDRESS (2)
// If a server wasn't reachable this many times, we slowly start skipping it on measurements
static const int FAIL_BACKOFF_START_COUNT = 8;
#define RTT_COUNT (4)

/* Module variables */

// Init guard
static bool connectionInitDone = false;
static bool threadInitDone = false;
pthread_mutex_t mutexInit = PTHREAD_MUTEX_INITIALIZER;
volatile bool keepRunning = true;
static bool learnNewServers;

// List of pending requests
static struct {
	dnbd3_async_t *head;
	dnbd3_async_t *tail;
	pthread_spinlock_t lock;
} requests;

// Connection for the image
static struct {
	char *name;
	uint16_t rid;
	uint64_t size;
} image;

static struct {
	int sockFd;
	pthread_mutex_t sendMutex;
	dnbd3_signal_t* panicSignal;
	dnbd3_host_t currentServer;
	ticks startupTime;
} connection;

// Known alt servers
typedef struct _alt_server {
	dnbd3_host_t host;
	atomic_int consecutiveFails;
	atomic_int rtt;
	int rtts[RTT_COUNT];
	int rttIndex;
	atomic_int bestCount;
	atomic_int liveRtt;
} alt_server_t;

static dnbd3_server_entry_t newservers[MAX_ALTS];
static pthread_mutex_t newAltLock = PTHREAD_MUTEX_INITIALIZER;
static alt_server_t altservers[MAX_ALTS];
// WR: Use when re-assigning or sorting altservers, i.e. an index in altservers
// changes its meaning (host). Also used for newservers.
// RD: Use when reading the list or modifying individual entries data, like RTT
// and fail count. Isn't super clean as we still might have races here, but mostly
// the code is clean in this regard, so we should only have stale data somewhere
// but nothing nonsensical.
static pthread_rwlock_t altLock = PTHREAD_RWLOCK_INITIALIZER;
#define lock_read pthread_rwlock_rdlock
#define lock_write pthread_rwlock_wrlock
#define unlock_rw pthread_rwlock_unlock

/* Static methods */


static void* connection_receiveThreadMain(void *sock);
static void* connection_backgroundThread(void *something);

static void addAltServers();
static void sortAltServers();
static void probeAltServers();
static void switchConnection(int sockFd, alt_server_t *srv);
static void requestAltServers();
static bool throwDataAway(int sockFd, uint32_t amount);

static void enqueueRequest(dnbd3_async_t *request);
static dnbd3_async_t* removeRequest(dnbd3_async_t *request);

bool connection_init(const char *hosts, const char *lowerImage, const uint16_t rid, const bool doLearnNew)
{
	int sock = -1;
	char host[SHORTBUF];
	size_t hlen;
	serialized_buffer_t buffer;
	uint16_t remoteVersion, remoteRid;
	char *remoteName;
	uint64_t remoteSize;
	struct sockaddr_storage sa;
	socklen_t salen;
	poll_list_t *cons = sock_newPollList();

	timing_setBase();
	pthread_mutex_lock( &mutexInit );
	if ( !connectionInitDone ) {
		dnbd3_host_t tempHosts[MAX_HOSTS_PER_ADDRESS];
		const char *current, *end;
		int altIndex = 0;
		learnNewServers = doLearnNew;
		memset( altservers, 0, sizeof altservers );
		connection.sockFd = -1;
		current = hosts;
		do {
			// Get next host from string
			while ( *current == ' ' ) current++;
			end = strchr( current, ' ' );
			size_t len = (end == NULL ? SHORTBUF : (size_t)( end - current ) + 1);
			if ( len > SHORTBUF ) len = SHORTBUF;
			snprintf( host, len, "%s", current );
			int newHosts = sock_resolveToDnbd3Host( host, tempHosts, MAX_HOSTS_PER_ADDRESS );
			for ( int i = 0; i < newHosts; ++i ) {
				if ( altIndex >= MAX_ALTS )
					break;
				altservers[altIndex].host = tempHosts[i];
				altIndex += 1;
			}
			current = end + 1;
		} while ( end != NULL && altIndex < MAX_ALTS );
		logadd( LOG_INFO, "Got %d servers from init call", altIndex );
		// Connect
		for ( int i = 0; i < altIndex + 5; ++i ) {
			if ( i >= altIndex ) {
				// Additional iteration - no corresponding slot in altservers, this
				// is just so we can make a final calls with longer timeout
				sock = sock_multiConnect( cons, NULL, 400, 3000 );
				if ( sock == -2 ) {
					logadd( LOG_ERROR, "Could not connect to any host" );
					sock = -1;
					break;
				}
			} else {
				if ( altservers[i].host.type == 0 )
					continue;
				// Try to connect - 100ms timeout
				sock = sock_multiConnect( cons, &altservers[i].host, 100, 3000 );
			}
			if ( sock == -2 || sock == -1 )
				continue;
			salen = sizeof(sa);
			if ( getpeername( sock, (struct sockaddr*)&sa, &salen ) == -1 ) {
				logadd( LOG_ERROR, "getpeername on successful connection failed!? (errno=%d)", errno );
				close( sock );
				sock = -1;
				continue;
			}
			hlen = sock_printable( (struct sockaddr*)&sa, salen, host, sizeof(host) );
			logadd( LOG_INFO, "Connected to %.*s", (int)hlen, host );
			if ( !dnbd3_select_image( sock, lowerImage, rid, 0 ) ) {
				logadd( LOG_ERROR, "Could not send select image" );
			} else if ( !dnbd3_select_image_reply( &buffer, sock, &remoteVersion, &remoteName, &remoteRid, &remoteSize ) ) {
				logadd( LOG_ERROR, "Could not read select image reply (%d)", errno );
			} else if ( rid != 0 && rid != remoteRid ) {
				logadd( LOG_ERROR, "rid mismatch (want: %d, got: %d)", (int)rid, (int)remoteRid );
			} else {
				logadd( LOG_INFO, "Requested: '%s:%d'", lowerImage, (int)rid );
				logadd( LOG_INFO, "Returned:  '%s:%d'", remoteName, (int)remoteRid );
				sock_setTimeout( sock, SOCKET_KEEPALIVE_TIMEOUT * 1000 );
				image.name = strdup( remoteName );
				image.rid = remoteRid;
				image.size = remoteSize;
				if ( !sock_sockaddrToDnbd3( (struct sockaddr*)&sa, &connection.currentServer ) ) {
					logadd( LOG_ERROR, "sockaddr to dnbd3_host_t failed!?" );
					connection.currentServer.type = 0;
				}
				connection.panicSignal = signal_new();
				timing_get( &connection.startupTime );
				connection.sockFd = sock;
				requests.head = NULL;
				requests.tail = NULL;
				requestAltServers();
				break;
			}
			// Failed
			if ( sock != -1 ) {
				close( sock );
				sock = -1;
			}
		}
		if ( sock != -1 ) {
			connectionInitDone = true;
		}
	}
	pthread_mutex_unlock( &mutexInit );
	sock_destroyPollList( cons );
	return sock != -1;
}

bool connection_initThreads()
{
	pthread_mutex_lock( &mutexInit );
	if ( !connectionInitDone || threadInitDone || connection.sockFd == -1 ) {
		pthread_mutex_unlock( &mutexInit );
		return false;
	}
	bool success = true;
	pthread_t thread;
	threadInitDone = true;
	logadd( LOG_DEBUG1, "Initializing stuff" );
	if ( pthread_mutex_init( &connection.sendMutex, NULL ) != 0
			|| pthread_spin_init( &requests.lock, PTHREAD_PROCESS_PRIVATE ) != 0 ) {
		logadd( LOG_ERROR, "Mutex or spinlock init failure" );
		success = false;
	} else {
		if ( pthread_create( &thread, NULL, &connection_receiveThreadMain, (void*)(size_t)connection.sockFd ) != 0 ) {
			logadd( LOG_ERROR, "Could not create receive thread" );
			success = false;
		} else if ( pthread_create( &thread, NULL, &connection_backgroundThread, NULL ) != 0 ) {
			logadd( LOG_ERROR, "Could not create background thread" );
			success = false;
		}
	}
	if ( !success ) {
		close( connection.sockFd );
		connection.sockFd = -1;
	}
	pthread_mutex_unlock( &mutexInit );
	return success;
}

uint64_t connection_getImageSize()
{
	return image.size;
}

bool connection_read(dnbd3_async_t *request)
{
	if ( !connectionInitDone ) return false;
	pthread_mutex_lock( &connection.sendMutex );
	enqueueRequest( request );
	if ( connection.sockFd != -1 ) {
		if ( !dnbd3_get_block( connection.sockFd, request->offset, request->length, (uint64_t)request, 0 ) ) {
			shutdown( connection.sockFd, SHUT_RDWR );
			connection.sockFd = -1;
			pthread_mutex_unlock( &connection.sendMutex );
			signal_call( connection.panicSignal );
			return true;
		}
	}
	pthread_mutex_unlock( &connection.sendMutex );
	return true;
}

void connection_close()
{
	if ( true ) {
		logadd( LOG_INFO, "Tearing down dnbd3 connections and workers" );
	}
	pthread_mutex_lock( &mutexInit );
	//keepRunning = false;
	if ( !connectionInitDone ) {
		pthread_mutex_unlock( &mutexInit );
		return;
	}
	pthread_mutex_unlock( &mutexInit );
	pthread_mutex_lock( &connection.sendMutex );
	if ( connection.sockFd != -1 ) {
		shutdown( connection.sockFd, SHUT_RDWR );
	}
	pthread_mutex_unlock( &connection.sendMutex );
	logadd( LOG_DEBUG1, "Connection closed." );
}

size_t connection_printStats(char *buffer, const size_t len)
{
	int ret;
	size_t remaining = len;
	declare_now;
	if ( remaining > 0 ) {
		ret = snprintf( buffer, remaining, "Image:    %s\nRevision: %d\n\nCurrent connection time: %" PRIu32 "s\n\n",
				image.name, (int)image.rid, timing_diff( &connection.startupTime, &now ) );
		if ( ret < 0 ) {
			ret = 0;
		}
		if ( (size_t)ret >= remaining ) {
			return len;
		}
		remaining -= ret;
		buffer += ret;
	}
	int i = -1;
	lock_read( &altLock );
	while ( remaining > 3 && ++i < MAX_ALTS ) {
		if ( altservers[i].host.type == 0 )
			continue;
		if ( isSameAddressPort( &connection.currentServer, &altservers[i].host ) ) {
			*buffer++ = '*';
		} else if ( i >= MAX_ALTS_ACTIVE ) {
			*buffer++ = '-';
		} else {
			*buffer++ = ' ';
		}
		const size_t addrlen = sock_printHost( &altservers[i].host, buffer, remaining );
		remaining -= (addrlen + 1); // For space or * above
		buffer += addrlen;
		if ( remaining < 3 )
			break;
		int width = addrlen >= 35 ? 0 : 35 - (int)addrlen;
		char *unit;
		int value;
		if ( altservers[i].rtt > 5000 ) {
			unit = "ms   ";
			value = altservers[i].rtt / 1000;
		} else {
			unit = "µs";
			value = altservers[i].rtt;
			width += 3;
		}
		ret = snprintf( buffer, remaining, "% *d %s   Unreachable:% 5d   BestCount:% 5d  Live:% 5dµs\n",
				width, value, unit, altservers[i].consecutiveFails, altservers[i].bestCount, altservers[i].liveRtt );
		if ( ret < 0 ) {
			ret = 0;
		}
		if ( (size_t)ret >= remaining ) {
			remaining = 0;
			break;
		}
		remaining -= ret;
		buffer += ret;
	}
	unlock_rw( &altLock );
	return len - remaining;
}

static void* connection_receiveThreadMain(void *sockPtr)
{
	int sockFd = (int)(size_t)sockPtr;
	dnbd3_reply_t reply;
	pthread_detach( pthread_self() );
	

	while ( true ) {
		int ret;
		struct fuse_bufvec splice_buf;
		do {
			ret = dnbd3_read_reply( sockFd, &reply, true );
			printf("\n ret %d %d", ret);
			if ( ret == REPLY_OK ) break;
		} while ( ret == REPLY_INTR || ret == REPLY_AGAIN );
		//printf("\n hallo %d", keepRunning);
		if ( ret != REPLY_OK ) {
			logadd( LOG_DEBUG1, "Error receiving reply on receiveThread (%d)", ret );
			if (ret == REPLY_CLOSED) return NULL;
			goto fail;
		}
		printf("\n Rep cmd %d", reply.cmd);
		if ( reply.cmd == CMD_GET_BLOCK ) {
			// Get block reply. find matching request
			dnbd3_async_t *request = removeRequest( (dnbd3_async_t*)reply.handle );
			if ( request == NULL ) {
				// This happens if the alt server probing thread tears down our connection
				// and did a direct RTT probe to satisfy this very request.
				logadd( LOG_DEBUG1, "Got block reply with no matching request" );
				if ( reply.size != 0 && !throwDataAway( sockFd, reply.size ) ) {
					logadd( LOG_DEBUG1, "....and choked on reply payload" );
					goto fail;
				}
			} else {
				// Found a match
				request->buffer = malloc(request->length); // char*
				if (request->mode == SPLICE) {
					splice_buf = FUSE_BUFVEC_INIT(request->length);
					splice_buf.buf[0].mem = request->buffer;
					splice_buf.buf[0].pos = request->offset;
				}
				const ssize_t ret = sock_recv( sockFd, request->buffer, request->length );
				if ( ret != (ssize_t)request->length ) {
					logadd( LOG_DEBUG1, "receiving payload for a block reply failed" );
					connection_read( request );
					free(request->buffer);
					request->buffer = NULL;
					goto fail;
				}
				// Check RTT
				declare_now;
				uint64_t diff = timing_diffUs( &request->time, &now );
				if ( diff < 30ull * 1000 * 1000 ) { // Sanity check - ignore if > 30s
					lock_read( &altLock );
					for ( int i = 0; i < MAX_ALTS; ++i ) {
						if ( altservers[i].host.type == 0 )
							continue;
						if ( isSameAddressPort( &connection.currentServer, &altservers[i].host ) ) {
							altservers[i].liveRtt = ( altservers[i].liveRtt * 3 + (int)diff ) / 4;
							break;
						}
					}
					unlock_rw( &altLock );
				}
				int rep = -1;
				if (request->mode == NO_SPLICE) {
					rep = fuse_reply_buf(request->fuse_req, request->buffer, request->length);
				}
				else if (request->mode == SPLICE) {
					rep = fuse_reply_data(request->fuse_req, &splice_buf, FUSE_BUF_FORCE_SPLICE);
					// free(splice_buf);
				}
				if (rep == 0) {
					// no error
				}
				else {
					printf("ERROR ON FUSE REPLY %i \n", rep);
					fuse_reply_err(request->fuse_req, rep);
				}
				free(request->buffer);
				request->buffer = NULL;
				free(request);
				request = NULL;
			}
		} else if ( reply.cmd == CMD_GET_SERVERS ) {
			// List of known alt servers
			dnbd3_server_entry_t entries[MAX_ALTS];
			const int count = MIN( reply.size / sizeof(dnbd3_server_entry_t), MAX_ALTS );
			const size_t relevantSize = sizeof(dnbd3_server_entry_t) * count;
			if ( sock_recv( sockFd, entries, relevantSize ) != (ssize_t)relevantSize
					|| !throwDataAway( sockFd, reply.size - (uint32_t)relevantSize ) ) {
				logadd( LOG_DEBUG1, "Error receiving list of alt servers." );
				goto fail;
			}
			pthread_mutex_lock( &newAltLock );
			memcpy( newservers, entries, relevantSize );
			pthread_mutex_unlock( &newAltLock );
		} else {
			// TODO: Handle the others?
			if ( reply.size != 0 && !throwDataAway( sockFd, reply.size ) ) {
				logadd( LOG_DEBUG1, "Could not throw %d bytes away on CMD %d", (int)reply.size, (int)reply.cmd );
				goto fail;
			}
		}
	}
	logadd( LOG_DEBUG1, "Aus der Schleife rausgeflogen! ARRRRRRRRRR" );
fail:;
	// Make sure noone is trying to use the socket for sending by locking,
	pthread_mutex_lock( &connection.sendMutex );
	// then just set the fd to -1, but only if it's the same fd as ours,
	// as someone could have established a new connection already
	if ( connection.sockFd == sockFd ) {
		connection.sockFd = -1;
		signal_call( connection.panicSignal );
	}
	pthread_mutex_unlock( &connection.sendMutex );
	// As we're the only reader, it's safe to close the socket now
	close( sockFd );
	return NULL;
}

static void* connection_backgroundThread(void *something UNUSED)
{
	ticks nextKeepalive;
	ticks nextRttCheck;

	timing_get( &nextKeepalive );
	nextRttCheck = nextKeepalive;
	while ( true) {
		//if ( !keepRunning ) break;
		ticks now;
		timing_get( &now );
		uint32_t wt1 = timing_diffMs( &now, &nextKeepalive );
		uint32_t wt2 = timing_diffMs( &now, &nextRttCheck );
		if ( wt1 > 0 && wt2 > 0 ) {
			int waitRes = signal_wait( connection.panicSignal, (int)MIN( wt1, wt2 ) + 1 );
			if ( waitRes == SIGNAL_ERROR ) {
				logadd( LOG_WARNING, "Error waiting on signal in background thread! Errno = %d", errno );
			}
			timing_get( &now );
		}
		// Woken up, see what we have to do
		const bool panic = connection.sockFd == -1;
		// Check alt servers
		if ( panic || timing_reachedPrecise( &nextRttCheck, &now ) ) {
			if ( learnNewServers ) {
				addAltServers();
			}
			sortAltServers();
			probeAltServers();
			if ( panic || timing_diff( &connection.startupTime, &now ) <= STARTUP_MODE_DURATION ) {
				timing_addSeconds( &nextRttCheck, &now, TIMER_INTERVAL_PROBE_STARTUP );
			} else {
				timing_addSeconds( &nextRttCheck, &now, TIMER_INTERVAL_PROBE_NORMAL );
			}
		}
		// Send keepalive packet
		if ( timing_reachedPrecise( &nextKeepalive, &now ) ) {
			pthread_mutex_lock( &connection.sendMutex );
			if ( connection.sockFd != -1 ) {
				dnbd3_request_t request;
				request.magic = dnbd3_packet_magic;
				request.cmd = CMD_KEEPALIVE;
				request.handle = request.offset = request.size = 0;
				fixup_request( request );
				ssize_t ret = sock_sendAll( connection.sockFd, &request, sizeof request, 2 );
				if ( (size_t)ret != sizeof request ) {
					shutdown( connection.sockFd, SHUT_RDWR );
					connection.sockFd = -1;
					nextRttCheck = now;
				}
			}
			pthread_mutex_unlock( &connection.sendMutex );
			timing_addSeconds( &nextKeepalive, &now, TIMER_INTERVAL_KEEPALIVE_PACKET );
		}
	}
	return NULL;
}

// Private quick helpers

static void addAltServers()
{
	pthread_mutex_lock( &newAltLock );
	lock_write( &altLock );
	for ( int nIdx = 0; nIdx < MAX_ALTS; ++nIdx ) {
		if ( newservers[nIdx].host.type == 0 )
			continue;
		// Got a new alt server, see if it's already known
		for ( int eIdx = 0; eIdx < MAX_ALTS; ++eIdx ) {
			if ( isSameAddress( &newservers[nIdx].host, &altservers[eIdx].host ) ) {
				goto skip_server;
			}
		}
		// Not known yet, add - find free slot
		int slot = -1;
		for ( int eIdx = 0; eIdx < MAX_ALTS; ++eIdx ) {
			if ( altservers[eIdx].host.type == 0 ) {
				slot = eIdx; // free - bail out and use this one
				break;
			}
			if ( altservers[eIdx].consecutiveFails > FAIL_BACKOFF_START_COUNT
					&& slot != -1 && altservers[slot].consecutiveFails < altservers[eIdx].consecutiveFails ) {
				// Replace an existing alt-server that failed recently if we got no more slots
				slot = eIdx;
			}
		}
		if ( slot != -1 ) {
			char txt[200];
			sock_printHost( &newservers[nIdx].host, txt, 200 );
			logadd( LOG_DEBUG1, "new server %s in slot %d", txt, slot );
			altservers[slot].consecutiveFails = 0;
			altservers[slot].bestCount = 0;
			altservers[slot].rtts[0] = RTT_UNREACHABLE;
			altservers[slot].rttIndex = 1;
			altservers[slot].host = newservers[nIdx].host;
			altservers[slot].liveRtt = 0;
		}
skip_server:;
	}
	memset( newservers, 0, sizeof(newservers) );
	unlock_rw( &altLock );
	pthread_mutex_unlock( &newAltLock );
}

/**
 * Find a server at index >= MAX_ALTS_ACTIVE (one that isn't considered for switching over)
 * that has been inactive for a while, then look if there's an active server that's failed
 * a couple of times recently. Swap both if found.
 */
static void sortAltServers()
{
	int ac = 0;
	lock_write( &altLock );
	for ( int ia = MAX_ALTS_ACTIVE; ia < MAX_ALTS; ++ia ) {
		alt_server_t * const inactive = &altservers[ia];
		if ( inactive->host.type == 0 || inactive->consecutiveFails > 0 )
			continue;
		while ( ac < MAX_ALTS_ACTIVE ) {
			if ( altservers[ac].host.type == 0 || altservers[ac].consecutiveFails > FAIL_BACKOFF_START_COUNT )
				break;
			ac++;
		}
		if ( ac == MAX_ALTS_ACTIVE )
			break;
		// Switch!
		alt_server_t * const active = &altservers[ac];
		dnbd3_host_t tmp = inactive->host;
		inactive->host = active->host;
		inactive->consecutiveFails = FAIL_BACKOFF_START_COUNT * 4;
		inactive->bestCount = 0;
		inactive->rtts[0] = RTT_UNREACHABLE;
		inactive->rttIndex = 1;
		inactive->liveRtt = 0;
		active->host = tmp;
		active->consecutiveFails = 0;
		active->bestCount = 0;
		active->rtts[0] = RTT_UNREACHABLE;
		active->rttIndex = 1;
		active->liveRtt = 0;
	}
	unlock_rw( &altLock );
}

static void probeAltServers()
{
	serialized_buffer_t buffer;
	dnbd3_reply_t reply;
	int bestSock = -1;
	uint16_t remoteRid, remoteProto;
	uint64_t remoteSize;
	char *remoteName;
	bool doSwitch;
	bool panic = connection.sockFd == -1;
	uint64_t testOffset = 0;
	uint32_t testLength = RTT_BLOCK_SIZE;
	dnbd3_async_t *request = NULL;
	alt_server_t *current = NULL, *best = NULL;

	if ( !panic ) {
		lock_read( &altLock );
		for ( int altIndex = 0; altIndex < MAX_ALTS; ++altIndex ) {
			if ( altservers[altIndex].host.type != 0
					&& isSameAddressPort( &altservers[altIndex].host, &connection.currentServer ) ) {
				current = &altservers[altIndex];
				break;
			}
		}
		unlock_rw( &altLock );
	}
	declare_now;
	pthread_spin_lock( &requests.lock );
	if ( requests.head != NULL ) {
		if ( !panic && current != NULL ) {
			const int maxDelay = MAX( current->rtt * 5, 1000000 ); // Give at least one second
			dnbd3_async_t *iterator;
			for ( iterator = requests.head; iterator != NULL; iterator = iterator->next ) {
				// A request with measurement tag is pending
				if ( timing_diffUs( &iterator->time, &now ) > maxDelay ) {
					panic = true;
					break;
				}
			}
		}
		if ( panic ) {
			request = requests.head;
			testOffset = requests.head->offset;
			testLength = requests.head->length;
		}
	}
	pthread_spin_unlock( &requests.lock );
	if ( testOffset != 0 ) {
		logadd( LOG_DEBUG1, "Panic with pending %" PRIu64 ":%" PRIu32, testOffset, testLength );
	}

	lock_read( &altLock );
	for ( int altIndex = 0; altIndex < (panic ? MAX_ALTS : MAX_ALTS_ACTIVE); ++altIndex ) {
		alt_server_t * const srv = &altservers[altIndex];
		if ( srv->host.type == 0 )
			continue;
		if ( !panic && srv->consecutiveFails > FAIL_BACKOFF_START_COUNT
				&& rand() % srv->consecutiveFails >= FAIL_BACKOFF_START_COUNT ) {
			continue;
		}
		if ( srv->rttIndex >= RTT_COUNT ) {
			srv->rttIndex = 0;
		} else {
			srv->rttIndex += 1;
		}
		// Probe
		ticks start;
		timing_get( &start );
		errno = 0;
		int sock = sock_connect( &srv->host, panic ? 1000 : 333, 1000 );
		if ( sock == -1 ) {
			logadd( LOG_DEBUG1, "Could not connect for probing. errno = %d", errno );
			goto fail;
		}
		if ( !dnbd3_select_image( sock, image.name, image.rid, 0 ) ) {
			logadd( LOG_DEBUG1, "probe: select_image failed" );
			goto fail;
		}
		if ( !dnbd3_select_image_reply( &buffer, sock, &remoteProto, &remoteName, &remoteRid, &remoteSize )) {
			logadd( LOG_DEBUG1, "probe: select image reply failed" );
			goto fail;
		}
		if ( remoteProto < MIN_SUPPORTED_SERVER ) {
			logadd( LOG_WARNING, "Unsupported remote version (local: %d, remote: %d)", (int)PROTOCOL_VERSION, (int)remoteProto );
			srv->consecutiveFails += 10;
			goto fail;
		}
		if ( remoteRid != image.rid || strcmp( remoteName, image.name ) != 0 ) {
			logadd( LOG_WARNING, "Remote rid or name mismatch (got '%s')", remoteName );
			srv->consecutiveFails += 10;
			goto fail;
		}
		if ( !dnbd3_get_block( sock, testOffset, testLength, 0, 0 ) ) {
			logadd( LOG_DEBUG1, "-> block request fail" );
			goto fail;
		}
		int a = 111;
		if ( !(a = dnbd3_get_reply( sock, &reply )) || reply.size != testLength ) {
			logadd( LOG_DEBUG1, "<- get block reply fail %d %d", a, (int)reply.size );
			goto fail;
		}
		if ( request != NULL && removeRequest( request ) != NULL ) {
			// Request successfully removed from queue
			const ssize_t ret = sock_recv( sock, request->buffer, request->length );
			if ( ret != (ssize_t)request->length ) {
				logadd( LOG_DEBUG1, "[RTT] receiving payload for a block reply failed" );
				// Failure, add to queue again
				connection_read( request );
				goto fail;
			}
			// Success, wake up caller
			logadd( LOG_DEBUG1, "[RTT] Successful direct probe" );
			//request->success = true;
			//request->finished = true;
			//signal_call( request->signal );
		} else {
			// Wasn't a request that's in our request queue
			if ( !throwDataAway( sock, testLength ) ) {
				logadd( LOG_DEBUG1, "<- get block reply payload fail" );
				goto fail;
			}
		}

		// Yay, success
		// Panic mode? Just switch to server
		if ( panic ) {
			unlock_rw( &altLock );
			switchConnection( sock, srv );
			return;
		}
		// Non-panic mode:
		// Update stats of server
		ticks end;
		timing_get( &end );
		srv->consecutiveFails = 0;
		srv->rtts[srv->rttIndex] = (int)timing_diffUs( &start, &end );
		int newRtt = 0;
		for ( int i = 0; i < RTT_COUNT; ++i ) {
			newRtt += srv->rtts[i];
		}
		if ( srv->liveRtt != 0 ) {
			// Make live rtt measurement influence result
			newRtt = ( newRtt + srv->liveRtt ) / ( RTT_COUNT + 1 );
		} else {
			newRtt /= RTT_COUNT;
		}
		srv->rtt = newRtt;

		// Keep socket open if this is currently the best one
		if ( best == NULL || best->rtt > srv->rtt ) {
			best = srv;
			if ( bestSock != -1 ) {
				close( bestSock );
			}
			bestSock = sock;
		} else {
			close( sock );
		}
		continue;
fail:;
		if ( sock != -1 ) {
			close( sock );
		}
		srv->rtts[srv->rttIndex] = RTT_UNREACHABLE;
		srv->consecutiveFails += 1;
	}
	doSwitch = false;
	if ( best != NULL ) {
		// Time-sensitive switch decision: If a server was best for some consecutive measurements,
		// we switch no matter how small the difference to the current server is
		for ( int altIndex = 0; altIndex < MAX_ALTS_ACTIVE; ++altIndex ) {
			alt_server_t * const srv = &altservers[altIndex];
			// Decay liveRtt slowly...
			if ( srv->liveRtt > current->liveRtt && srv->liveRtt > srv->rtt ) {
				srv->liveRtt -= ( ( srv->liveRtt / 100 ) + 1 );
			}
			if ( srv == best ) {
				if ( srv->bestCount < 50 ) {
					srv->bestCount += 2;
				}
				// Switch with increasing probability the higher the bestCount is
				if ( srv->bestCount > 12 && ( current == NULL || srv->rtt < current->rtt ) && srv->bestCount > rand() % 50 ) {
					doSwitch = true;
				}
			} else if ( srv->bestCount > 0 ) {
				srv->bestCount--;
			}
		}
		for ( int i = MAX_ALTS_ACTIVE; i < MAX_ALTS; ++i ) {
			if ( altservers[i].consecutiveFails > 0 ) {
				altservers[i].consecutiveFails--;
			}
		}
		// This takes care of the situation where two servers alternate being the best server all the time
		if ( doSwitch && current != NULL && best->bestCount - current->bestCount < 8 ) {
			doSwitch = false;
		}
		// Regular logic: Apply threshold when considering switch
		if ( !doSwitch && current != NULL ) {
			doSwitch = current->rtt > best->rtt + RTT_ABSOLUTE_THRESHOLD
					|| RTT_THRESHOLD_FACTOR(current->rtt) > best->rtt + 1000;
		}
	}
	// Switch if a better server was found
	if ( doSwitch ) {
		logadd( LOG_INFO, "Current: %dµs, best: %dµs. Will switch!", current == NULL ? 0 : current->rtt, best->rtt );
		for ( int i = 0; i < MAX_ALTS; ++i ) {
			if ( &altservers[i] != best ) {
				altservers[i].bestCount = 0;
			}
		}
		unlock_rw( &altLock );
		switchConnection( bestSock, best );
		return;
	}
	// No switch
	unlock_rw( &altLock );
	if ( best != NULL ) {
		close( bestSock );
	}
}

static void switchConnection(int sockFd, alt_server_t *srv)
{
	pthread_t thread;
	struct sockaddr_storage addr;
	socklen_t addrLen = sizeof(addr);
	char message[200] = "Connection switched to ";
	const size_t len = strlen( message );
	int ret;
	dnbd3_async_t *queue, *it;

	pthread_mutex_lock( &connection.sendMutex );
	if ( connection.sockFd != -1 ) {
		shutdown( connection.sockFd, SHUT_RDWR );
	}
	ret = getpeername( sockFd, (struct sockaddr*)&addr, &addrLen );
	if ( ret == 0 ) {
		connection.currentServer = srv->host;
		connection.sockFd = sockFd;
		pthread_spin_lock( &requests.lock );
		queue = requests.head;
		requests.head = requests.tail = NULL;
		pthread_spin_unlock( &requests.lock );
	} else {
		connection.sockFd = -1;
	}
	requestAltServers();
	pthread_mutex_unlock( &connection.sendMutex );
	if ( ret != 0 ) {
		close( sockFd );
		logadd( LOG_WARNING, "Could not getpeername after connection switch, assuming connection already dead again. (Errno=%d)", errno );
		signal_call( connection.panicSignal );
		return;
	}
	timing_get( &connection.startupTime );
	pthread_create( &thread, NULL, &connection_receiveThreadMain, (void*)(size_t)sockFd );
	sock_printable( (struct sockaddr*)&addr, sizeof(addr), message + len, sizeof(message) - len );
	logadd( LOG_INFO, "%s", message );
	// resend queue
	if ( queue != NULL ) {
		pthread_mutex_lock( &connection.sendMutex );
		dnbd3_async_t *next = NULL;
		for ( it = queue; it != NULL; it = next ) {
			logadd( LOG_DEBUG1, "Requeue after server change" );
			next = it->next;
			enqueueRequest( it );
			if ( connection.sockFd != -1 && !dnbd3_get_block( connection.sockFd, it->offset, it->length, (uint64_t)it, 0 ) ) {
				logadd( LOG_WARNING, "Resending pending request failed, re-entering panic mode" );
				shutdown( connection.sockFd, SHUT_RDWR );
				connection.sockFd = -1;
				signal_call( connection.panicSignal );
			}
		}
		pthread_mutex_unlock( &connection.sendMutex );
	}
}

/**
 * Does not lock, so get the sendMutex first!
 */
static void requestAltServers()
{
	if ( connection.sockFd == -1 || !learnNewServers )
		return;
	dnbd3_request_t request = { 0 };
	request.magic = dnbd3_packet_magic;
	request.cmd = CMD_GET_SERVERS;
	fixup_request( request );
	if ( sock_sendAll( connection.sockFd, &request, sizeof(request), 2 ) != (ssize_t)sizeof(request) ) {
		logadd( LOG_WARNING, "Connection failed while requesting alt server list" );
		shutdown( connection.sockFd, SHUT_RDWR );
		connection.sockFd = -1;
	}
}

static bool throwDataAway(int sockFd, uint32_t amount)
{
	size_t done = 0;
	char tempBuffer[SHORTBUF];
	while ( done < amount ) {
		const ssize_t ret = sock_recv( sockFd, tempBuffer, MIN( amount - done, SHORTBUF ) );
		if ( ret <= 0 )
			return false;
		done += (size_t)ret;
	}
	return true;
}

static void enqueueRequest(dnbd3_async_t *request)
{
	request->next = NULL;
	//request->finished = false;
	//request->success = false;
	//logadd( LOG_DEBUG2, "Queue: %p @ %s : %d", request, file, line );
	// Measure latency and add to switch formula
	timing_get( &request->time );
	pthread_spin_lock( &requests.lock );
	if ( requests.head == NULL ) {
		requests.head = requests.tail = request;
	} else {
		requests.tail->next = request;
		requests.tail = request;
	}
	pthread_spin_unlock( &requests.lock );
}

static dnbd3_async_t* removeRequest(dnbd3_async_t *request)
{
	pthread_spin_lock( &requests.lock );
	//logadd( LOG_DEBUG2, "Remov: %p @ %s : %d", request, file, line );
	dnbd3_async_t *iterator, *prev = NULL;
	for ( iterator = requests.head; iterator != NULL; iterator = iterator->next ) {
		if ( iterator == request ) {
			// Found it, break!
			if ( prev != NULL ) {
				prev->next = iterator->next;
			} else {
				requests.head = iterator->next;
			}
			if ( requests.tail == iterator ) {
				requests.tail = prev;
			}
			break;
		}
		prev = iterator;
	}
	pthread_spin_unlock( &requests.lock );
	return iterator;
}

