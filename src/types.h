/*
 * This file is part of the Distributed Network Block Device 3
 *
 * Copyright(c) 2011-2012 Johann Latocha <johann@latocha.de>
 *
 * This file may be licensed under the terms of of the
 * GNU General Public License Version 2 (the ``GPL'').
 *
 * Software distributed under the License is distributed
 * on an ``AS IS'' basis, WITHOUT WARRANTY OF ANY KIND, either
 * express or implied. See the GPL for the specific language
 * governing rights and limitations.
 *
 * You should have received a copy of the GPL along with this
 * program. If not, go to http://www.gnu.org/licenses/gpl.html
 * or write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#ifndef TYPES_H_
#define TYPES_H_

#include "config.h"
#ifndef KERNEL_MODULE
#include <stdint.h>
#include <stdbool.h>
#endif

#ifndef MIN
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif

#ifdef __GNUC__
#define UNUSED __attribute__ ((unused))
#else
#error "Please add define for your compiler for UNUSED, or define to nothing for your compiler if not supported"
#endif

#if defined(__GNUC__) && __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

#ifdef __linux__
#define HAVE_THREAD_NAMES
#endif

#ifdef __FreeBSD__
#ifndef MSG_MORE
#define MSG_MORE 0
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif
#include <netinet/in.h>
#endif

#ifdef AFL_MODE
#define send(a,b,c,d) write(a,b,c)
#define recv(a,b,c,d) read(a,b,c)
#endif


// ioctl
#define DNBD3_MAGIC     'd'
#define IOCTL_OPEN      _IO(0xab, 1)
#define IOCTL_CLOSE     _IO(0xab, 2)
#define IOCTL_SWITCH    _IO(0xab, 3)
#define IOCTL_ADD_SRV	_IO(0xab, 4)
#define IOCTL_REM_SRV	_IO(0xab, 5)

#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER) && defined(__BIG_ENDIAN) && __BYTE_ORDER == __BIG_ENDIAN) || (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static const uint16_t dnbd3_packet_magic = (0x73 << 8) | (0x72);
// Flip bytes around on big endian when putting stuff on the net
#define net_order_64(a) ((uint64_t)((((a) & 0xFFull) << 56) | (((a) & 0xFF00ull) << 40) | (((a) & 0xFF0000ull) << 24) | (((a) & 0xFF000000ull) << 8) | (((a) & 0xFF00000000ull) >> 8) | (((a) & 0xFF0000000000ull) >> 24) | (((a) & 0xFF000000000000ull) >> 40) | (((a) & 0xFF00000000000000ull) >> 56)))
#define net_order_32(a) ((uint32_t)((((a) & (uint32_t)0xFF) << 24) | (((a) & (uint32_t)0xFF00) << 8) | (((a) & (uint32_t)0xFF0000) >> 8) | (((a) & (uint32_t)0xFF000000) >> 24)))
#define net_order_16(a) ((uint16_t)((((a) & (uint16_t)0xFF) << 8) | (((a) & (uint16_t)0xFF00) >> 8)))
#define fixup_request(a) do { \
	(a).cmd = net_order_16((a).cmd); \
	(a).size = net_order_32((a).size); \
	(a).offset = net_order_64((a).offset); \
} while (0)
#define fixup_reply(a) do { \
	(a).cmd = net_order_16((a).cmd); \
	(a).size = net_order_32((a).size); \
} while (0)
#define ENDIAN_MODE "Big Endian"
#ifndef BIG_ENDIAN
#define BIG_ENDIAN
#endif
#elif defined(__LITTLE_ENDIAN__) || (defined(__BYTE_ORDER) && defined(__LITTLE_ENDIAN) && __BYTE_ORDER == __LITTLE_ENDIAN) || (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(__i386__) || defined(__i386) || defined(__x86_64)
static const uint16_t dnbd3_packet_magic = (0x73) | (0x72 << 8);
// Make little endian our network byte order as probably 99.999% of machines this will be used on are LE
#define net_order_64(a) (a)
#define net_order_32(a) (a)
#define net_order_16(a) (a)
#define fixup_request(a) while(0)
#define fixup_reply(a)   while(0)
#define ENDIAN_MODE "Little Endian"
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN
#endif
#else
#error "Unknown Endianness"
#endif

typedef uint8_t dnbd3_af;

static const dnbd3_af HOST_NONE = (dnbd3_af)0;
static const dnbd3_af HOST_IP4 = (dnbd3_af)2;
static const dnbd3_af HOST_IP6 = (dnbd3_af)10;

#pragma pack(1)
typedef struct dnbd3_host_t
{
	uint8_t addr[16];    // 16byte (network representation, so it can be directly passed to socket functions)
	uint16_t port;       // 2byte (network representation, so it can be directly passed to socket functions)
	dnbd3_af type;        // 1byte (ip version. HOST_IP4 or HOST_IP6. 0 means this struct is empty and should be ignored)
} dnbd3_host_t;
#pragma pack(0)

#pragma pack(1)
typedef struct
{
	uint16_t len;
	dnbd3_host_t host;
	uint16_t imgnamelen;
	char *imgname;
	int rid;
	int read_ahead_kb;
	uint8_t use_server_provided_alts;
} dnbd3_ioctl_t;
#pragma pack(0)

// network
#define CMD_GET_BLOCK           1
#define CMD_SELECT_IMAGE        2
#define CMD_GET_SERVERS         3
#define CMD_ERROR               4
#define CMD_KEEPALIVE           5
#define CMD_LATEST_RID          6
#define CMD_SET_CLIENT_MODE     7
#define CMD_GET_CRC32           8

#define DNBD3_REQUEST_SIZE     24
#pragma pack(1)
typedef struct
{
	uint16_t magic;           // 2byte
	uint16_t cmd;             // 2byte
	uint32_t size;            // 4byte
	union {
		struct {
#ifdef LITTLE_ENDIAN
			uint64_t offset_small:56;  // 7byte
			uint8_t  hops;            // 1byte
#elif defined(BIG_ENDIAN)
			uint8_t  hops;            // 1byte
			uint64_t offset_small:56;  // 7byte
#endif
		};
		uint64_t offset;       // 8byte
	};
	uint64_t handle;          // 8byte
} dnbd3_request_t;
#pragma pack(0)
_Static_assert( sizeof(dnbd3_request_t) == DNBD3_REQUEST_SIZE, "dnbd3_request_t is messed up" );

#define DNBD3_REPLY_SIZE       16
#pragma pack(1)
typedef struct
{
	uint16_t magic;		// 2byte
	uint16_t cmd;		// 2byte
	uint32_t size;		// 4byte
	uint64_t handle;	// 8byte
} dnbd3_reply_t;
#pragma pack(0)
_Static_assert( sizeof(dnbd3_reply_t) == DNBD3_REPLY_SIZE, "dnbd3_reply_t is messed up" );

#pragma pack(1)
typedef struct
{
	dnbd3_host_t host;
	uint8_t  failures;		// 1byte (number of times server has been consecutively unreachable)
} dnbd3_server_entry_t;
#pragma pack(0)

#endif /* TYPES_H_ */
