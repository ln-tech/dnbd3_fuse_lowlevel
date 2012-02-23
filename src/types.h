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

// ioctl
#define DNBD3_MAGIC     'd'
#define IOCTL_OPEN      _IO(0xab, 1)
#define IOCTL_CLOSE     _IO(0xab, 2)
#define IOCTL_SWITCH    _IO(0xab, 3)

typedef struct
{
    char *host;
    char *port;
    int vid;
    int rid;
    int read_ahead_kb;
} dnbd3_ioctl_t;

// network
#define CMD_GET_BLOCK   1
#define CMD_GET_SIZE    2
#define CMD_GET_SERVERS 3

#pragma pack(1)
typedef struct
{
    uint16_t cmd;       // 2byte
    uint16_t vid;       // 2byte
    uint16_t rid;       // 2byte
    uint64_t offset;    // 8byte
    uint64_t size;      // 8byte
    char handle[8];     // 8byte
} dnbd3_request_t;
#pragma pack(0)

#pragma pack(1)
typedef struct
{
    uint16_t cmd;   // 2byte
    uint16_t vid;   // 2byte
    uint16_t rid;   // 2byte
    uint64_t size;  // 8byte
    char handle[8]; // 8byte
} dnbd3_reply_t;
#pragma pack(0)

#endif /* TYPES_H_ */
