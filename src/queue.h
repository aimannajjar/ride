#include "ride.h"

#ifndef QUEUE_H
#define QUEUE_H

#define QUEUE_SIZE 8192

void queue_init(void);
int queue_add(struct event *);
int queue_consume(struct event *event);

#endif // QUEUE_H
