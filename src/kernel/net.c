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

#include "net.h"
#include "blk.h"
#include "utils.h"

void dnbd3_net_connect(dnbd3_device_t *dev)
{
    struct sockaddr_in sin;
    struct request *req = kmalloc(sizeof(struct request), GFP_ATOMIC);

    // do some checks before connecting
    if (!req)
    {
        printk("ERROR: Kmalloc failed.\n");
        return;
    }
    if (!dev->host || !dev->port || (dev->vid == 0))
    {
        printk("ERROR: Host, port or vid not set.\n");
        return;
    }
    if (dev->sock)
    {
        printk("ERROR: Device %s already connected to %s.\n", dev->disk->disk_name, dev->host);
        return;
    }

    printk("INFO: Connecting device %s to %s\n", dev->disk->disk_name, dev->host);

    // initialize socket
    if (sock_create_kern(AF_INET, SOCK_STREAM, IPPROTO_TCP, &dev->sock) < 0)
    {
        printk("ERROR: Couldn't create socket.\n");
        dev->sock = NULL;
        return;
    }
    dev->sock->sk->sk_allocation = GFP_NOIO;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(dev->host);
    sin.sin_port = htons(simple_strtol(dev->port, NULL, 10));
    if (kernel_connect(dev->sock, (struct sockaddr *) &sin, sizeof(sin), 0) < 0)
    {
        printk("ERROR: Couldn't connect to host %s:%s\n", dev->host, dev->port);
        dev->sock = NULL;
        return;
    }

    // start sending thread
    dev->thread_send = kthread_create(dnbd3_net_send, dev, dev->disk->disk_name);
    wake_up_process(dev->thread_send);

    // start receiving thread
    dev->thread_receive = kthread_create(dnbd3_net_receive, dev, dev->disk->disk_name);
    wake_up_process(dev->thread_receive);

    // add heartbeat timer
    init_timer(&dev->hb_timer);
    dev->hb_timer.data = (unsigned long) dev;
    dev->hb_timer.function = dnbd3_net_heartbeat;
    dev->hb_timer.expires = jiffies + HB_INTERVAL;
    add_timer(&dev->hb_timer);

    // enqueue request to request_queue_send (ask file size)
    req->cmd_type = REQ_TYPE_SPECIAL;
    req->cmd_flags = REQ_GET_FILESIZE;
    list_add(&req->queuelist, &dev->request_queue_send);
    wake_up(&dev->process_queue_send);
}

void dnbd3_net_disconnect(dnbd3_device_t *dev)
{
    struct request *blk_request, *tmp_request;
    printk("INFO: Disconnecting device %s\n", dev->disk->disk_name);

    // kill sending and receiving threads
    if (dev->thread_send && dev->thread_receive)
    {
        kthread_stop(dev->thread_send);
        kthread_stop(dev->thread_receive);
    }

    // clear socket
    if (dev->sock)
    {
        sock_release(dev->sock);
        dev->sock = NULL;
    }
    // clear heartbeat timer
    if (&dev->hb_timer)
        del_timer(&dev->hb_timer);

    // move already send requests to request_queue_send again
    if (!list_empty(&dev->request_queue_receive))
    {
        printk("WARN: Request queue was not empty on %s\n", dev->disk->disk_name);
        spin_lock_irq(&dev->blk_lock);
        list_for_each_entry_safe(blk_request, tmp_request, &dev->request_queue_receive, queuelist)
        {
            list_del_init(&blk_request->queuelist);
            list_add_tail(&blk_request->queuelist, &dev->request_queue_send);
        }
        spin_unlock_irq(&dev->blk_lock);
    }
}

int dnbd3_net_send(void *data)
{
    dnbd3_device_t *dev = data;
    struct request *blk_request;

    dnbd3_request_t dnbd3_request;
    struct msghdr msg;
    struct kvec iov;

    init_msghdr(msg);
    set_user_nice(current, -20);

    while (!kthread_should_stop() || !list_empty(&dev->request_queue_send))
    {
        wait_event_interruptible(dev->process_queue_send,
                kthread_should_stop() || !list_empty(&dev->request_queue_send));

        if (list_empty(&dev->request_queue_send))
            continue;

        // extract block request
        spin_lock_irq(&dev->blk_lock);
        blk_request = list_entry(dev->request_queue_send.next, struct request, queuelist);
        list_del_init(&blk_request->queuelist);
        spin_unlock_irq(&dev->blk_lock);

        // what to do?
        switch (blk_request->cmd_type)
        {
        case REQ_TYPE_FS:
            dnbd3_request.cmd = CMD_GET_BLOCK;
            dnbd3_request.offset = blk_rq_pos(blk_request) << 9; // *512
            dnbd3_request.size = blk_rq_bytes(blk_request); // bytes left to complete entire request
            break;

        case REQ_TYPE_SPECIAL:
            switch (blk_request->cmd_flags)
            {
            case REQ_GET_FILESIZE:
                dnbd3_request.cmd = CMD_GET_SIZE;
                dnbd3_request.vid = dev->vid;
                dnbd3_request.rid = dev->rid;
                break;
            case REQ_GET_SERVERS:
                dnbd3_request.cmd = CMD_GET_SERVERS;
                break;
            }
            break;

        default:
            printk("ERROR: Unknown command (Send)\n");
            continue;
        }

        // send net request
        memcpy(dnbd3_request.handle, &blk_request, sizeof(blk_request));
        iov.iov_base = &dnbd3_request;
        iov.iov_len = sizeof(dnbd3_request);
        if (kernel_sendmsg(dev->sock, &msg, &iov, 1, sizeof(dnbd3_request)) <= 0)
            printk("ERROR: kernel_sendmsg\n");

        // enqueue request to request_queue_receive
        spin_lock_irq(&dev->blk_lock);
        list_add_tail(&blk_request->queuelist, &dev->request_queue_receive);
        spin_unlock_irq(&dev->blk_lock);
        wake_up(&dev->process_queue_receive);
    }
    return 0;
}

int dnbd3_net_receive(void *data)
{
    dnbd3_device_t *dev = data;
    struct request *blk_request, *tmp_request, *received_request;

    dnbd3_reply_t dnbd3_reply;
    struct msghdr msg;
    struct kvec iov;
    struct req_iterator iter;
    struct bio_vec *bvec;
    void *kaddr;
    unsigned long flags;
    sigset_t blocked, oldset;

    unsigned int size, i;
    uint64_t filesize;

    init_msghdr(msg);
    set_user_nice(current, -20);

    while (!kthread_should_stop() || !list_empty(&dev->request_queue_receive))
    {
        wait_event_interruptible(dev->process_queue_receive,
                kthread_should_stop() || !list_empty(&dev->request_queue_receive));

        if (list_empty(&dev->request_queue_receive))
            continue;

        // receive net replay
        iov.iov_base = &dnbd3_reply;
        iov.iov_len = sizeof(dnbd3_reply);
        kernel_recvmsg(dev->sock, &msg, &iov, 1, sizeof(dnbd3_reply), msg.msg_flags);

        // search for replied request in queue
        received_request = *(struct request **) dnbd3_reply.handle;
        spin_lock_irq(&dev->blk_lock);
        list_for_each_entry_safe(blk_request, tmp_request, &dev->request_queue_receive, queuelist)
        {
            if (blk_request != received_request)
                continue;

            list_del_init(&blk_request->queuelist);
            break;
        }
        spin_unlock_irq(&dev->blk_lock);

        // check if server send error
        switch (dnbd3_reply.error)
        {
        case ERROR_SIZE:
            printk("ERROR: Requested image does't exist\n");
            kthread_stop(dev->thread_send);
            del_timer(&dev->hb_timer);
            sock_release(dev->sock);
            kfree(blk_request);
            dev->sock = NULL;
            return -1;

        case ERROR_RELOAD:
            blk_request->cmd_type = REQ_TYPE_SPECIAL;
            blk_request->cmd_flags = REQ_GET_FILESIZE;
            list_add(&blk_request->queuelist, &dev->request_queue_send);
            wake_up(&dev->process_queue_send);
            continue;

        }

        // what to do?
        switch (dnbd3_reply.cmd)
        {
        case CMD_GET_BLOCK:
            // receive data and answer to block layer
            rq_for_each_segment(bvec, blk_request, iter)
            {
                siginitsetinv(&blocked, sigmask(SIGKILL));
                sigprocmask(SIG_SETMASK, &blocked, &oldset);

                kaddr = kmap(bvec->bv_page) + bvec->bv_offset;
                size = bvec->bv_len;
                iov.iov_base = kaddr;
                iov.iov_len = size;
                kernel_recvmsg(dev->sock, &msg, &iov, 1, size, msg.msg_flags);
                kunmap(bvec->bv_page);

                sigprocmask(SIG_SETMASK, &oldset, NULL);
            }
            spin_lock_irqsave(&dev->blk_lock, flags);
            __blk_end_request_all(blk_request, 0);
            spin_unlock_irqrestore(&dev->blk_lock, flags);
            continue;

        case CMD_GET_SIZE:
            iov.iov_base = &filesize;
            iov.iov_len = sizeof(uint64_t);
            kernel_recvmsg(dev->sock, &msg, &iov, 1, dnbd3_reply.size, msg.msg_flags);
            set_capacity(dev->disk, filesize >> 9); /* 512 Byte blocks */
            printk("INFO: Filesize %s: %llu\n", dev->disk->disk_name, filesize);
            kfree(blk_request);
            continue;

        case CMD_GET_SERVERS:
            dev->num_servers = dnbd3_reply.size / sizeof(struct in_addr);
            size = sizeof(struct in_addr);
            for (i = 0; i < dev->num_servers && i < MAX_NUMBER_SERVERS; i++)
            {
                iov.iov_base = &dev->servers[i];
                iov.iov_len = size;
                kernel_recvmsg(dev->sock, &msg, &iov, 1, size, msg.msg_flags);
            }
            kfree(blk_request);
            continue;

        default:
            printk("ERROR: Unknown command (Receive)\n");
            continue;

        }
    }
    return 0;
}

void dnbd3_net_heartbeat(unsigned long arg)
{
    dnbd3_device_t *dev = (dnbd3_device_t *) arg;
    struct request *req = kmalloc(sizeof(struct request), GFP_ATOMIC);
    if (req)
    {
        req->cmd_type = REQ_TYPE_SPECIAL;
        req->cmd_flags = REQ_GET_SERVERS;
        list_add(&req->queuelist, &dev->request_queue_send);
        wake_up(&dev->process_queue_send);
    }
    dev->hb_timer.expires = jiffies + HB_INTERVAL;
    add_timer(&dev->hb_timer);
}
