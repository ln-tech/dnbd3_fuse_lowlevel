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

#ifndef BLK_H_
#define BLK_H_

#include "dnbd3.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
#define REQ_TYPE_SPECIAL REQ_TYPE_DRV_PRIV
#endif

extern struct block_device_operations dnbd3_blk_ops;

int dnbd3_blk_ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd, unsigned long arg);

void dnbd3_blk_request(struct request_queue *q);

int dnbd3_blk_add_device(dnbd3_device_t *dev, int minor);

int dnbd3_blk_del_device(dnbd3_device_t *dev);

void dnbd3_blk_fail_all_requests(dnbd3_device_t *dev);

#endif /* BLK_H_ */
