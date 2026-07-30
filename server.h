#ifndef SERVER_H
#define SERVER_H
#include "dispatcher.h"
void server_start(int port);
void server_broadcast(const char *msg);
void server_on_sensor(EventType type,int value);
#endif

