#ifndef _UPLINK_H_
#define _UPLINK_H_

#include "../types.h"
#include "globals.h"

bool uplink_init(dnbd3_image_t *image, int sock, dnbd3_host_t *host);

void uplink_removeClient(dnbd3_connection_t *uplink, dnbd3_client_t *client);

bool uplink_request(dnbd3_client_t *client, uint64_t handle, uint64_t start, uint32_t length);

void uplink_shutdown(dnbd3_image_t *image);

#endif /* UPLINK_H_ */
