#ifndef CLIENT_H
#define CLIENT_H

#include <quiche.h>
#include "ride.h"
#include "worker.h"

struct client {
  // quiche structs
  quiche_conn *quiche_conn;
  quiche_config *quiche_config;

  // socket addresses
  struct sockaddr_storage local_addr;
  struct sockaddr remote_addr;
  socklen_t remote_addr_len;
  socklen_t local_addr_len;

  // conn/sock id
  uint8_t scid[16];
  int sock_fd;
};


struct verifier_config client_verifier_config();
void client_init(struct client *client, struct job_generic *init_job);
int client_verify_request(struct client *client);

#endif // CLIENT_H
