#include "ride.h"
#include <stdint.h>

#ifndef QUEUE_H
#define QUEUE_H

#define QUEUE_SIZE 2048

void queue_init(void);
int queue_add(struct event *);
int queue_consume(struct event *, uint64_t timeout_ms);
int queue_consume_try(struct event *event);

#endif // QUEUE_H
