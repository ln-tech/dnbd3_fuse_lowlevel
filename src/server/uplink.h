#ifndef _UPLINK_H_
#define _UPLINK_H_

#include "../types.h"
#include "globals.h"

extern dnbd3_alt_server_t *_alt_servers[SERVER_MAX_ALTS];
extern int _num_alts;
extern pthread_spinlock_t _alts_lock;

int uplink_get_matching_alt_servers(dnbd3_host_t *host, dnbd3_server_entry_t *output, int size);

int uplink_net_closeness(dnbd3_host_t *host1, dnbd3_host_t *host2);

int uplink_init(dnbd3_image_t *image);

void uplink_shutdown( dnbd3_connection_t *uplink);

#endif /* UPLINK_H_ */
