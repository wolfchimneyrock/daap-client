#ifndef __DAAP_CLIENT_H__
#define __DAAP_CLIENT_H__
#include <pthread.h>

typedef struct config_t {
	int   port;
	int   threads;
    int   timeout;
    int   verbose;
    int   count;
    int   limit;
    int   active;
    uint64_t mac;
	char *host;
} config_t;


typedef struct test_t {
    //char dummy[256];
    int thread_id;
    int connection_id;
    int song_id;
    int started;
    size_t size;
    int request_id;
    size_t retries;
    size_t delay;
    struct timeval start, responded, end;
    evhtp_connection_t *conn;
    struct event       *timer;
    evhtp_request_t    *req;
    evbase_t           *base;
    //uint64_t dummy2[4096];
} test_t;

typedef struct app_t {
    int thread_id;
    int pid;
    int count;
    config_t *conf;
} app_t;

#define TIMESTAMP(t) \
        (1000000 * t.tv_sec + t.tv_usec)
#endif
