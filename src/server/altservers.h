#ifndef _ALTSERVERS_H_
#define _ALTSERVERS_H_

#include "globals.h"

struct json_t;

void altservers_init();

void altservers_shutdown();

int altservers_load();

bool altservers_add(dnbd3_host_t *host, const char *comment, const int isPrivate, const int isClientOnly);

void altservers_findUplink(dnbd3_connection_t *uplink);

void altservers_removeUplink(dnbd3_connection_t *uplink);

int altservers_getListForClient(dnbd3_host_t *host, dnbd3_server_entry_t *output, int size);

int altservers_getListForUplink(dnbd3_host_t *output, int size, int emergency);

int altservers_netCloseness(dnbd3_host_t *host1, dnbd3_host_t *host2);

void altservers_serverFailed(const dnbd3_host_t * const host);

struct json_t* altservers_toJson();

#endif /* UPLINK_CONNECTOR_H_ */
