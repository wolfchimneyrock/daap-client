#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h> 
#include <unistd.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <evhtp/evhtp.h>
#include <event2/util.h>
#include <getopt.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/stat.h>
#include <signal.h>
#include <math.h>
#include <thread.h>
#include <float.h>

#include "daap-client.h"

#define TICK 1000000
#define CLT_COUNT 6
#define TEST_STR "/databases/1/items/%d"
#define MAX_RETRY 10
#define TWOPI 2*3.14159265358979323846

#define TIMESTAMP(t) \
    (1000000 * t.tv_sec + t.tv_usec)

#define GLOBAL_ADD(g, v) \
{ while (!__sync_bool_compare_and_swap(&(g), g, g + v)); }

static config_t  conf;
static uint64_t bytes_since_tick = 0;
// static pthread_mutex_t   bytes_since_tick_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t      updates_since_tick = 0;
static uint64_t      accumulated_active = 0;
static uint64_t      accumulated_latency = 0;
static uint64_t      currently_active = 0;
// static pthread_mutex_t   currently_active_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t main_pid, signal_pid, timer_pid, *pids;


static char localhost[256];
static app_t    *apps;

static uint64_t mac_address() {
    // taken from https://stackoverflow.com/a/1779758
    // retrieve the mac address of the first non-loopback inet interface
    struct ifreq ifr;
    struct ifconf ifc;
    char buf[1024];
    int success = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1) { goto error; };

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1) { /* handle error */ }

    struct ifreq* it = ifc.ifc_req;
    const struct ifreq* const end = it + (ifc.ifc_len / sizeof(struct ifreq));

    for (; it != end; ++it) {
        strcpy(ifr.ifr_name, it->ifr_name);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (! (ifr.ifr_flags & IFF_LOOPBACK)) { // don't count loopback
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    success = 1;
                    break;
                }
            }
        }
        else { goto error; }
    }

    unsigned char mac_address[8] = {0};

    if (success) memcpy(mac_address, ifr.ifr_hwaddr.sa_data, 6);
    uint64_t *x = (uint64_t *)mac_address;
    return *x;
error:
    return 0;
}

static void print_test(test_t *t) {
    fprintf(stderr, ("\t\tthread     %d\n"
                     "\t\tconnection %d\n"
                     "\t\trequest    %d\n"
                     "\t\tsong       %d\n"
                     "\t\tstarted    %d\n"
                     "\t\tsize       %lu\n"
                     "\t\ttimer      %p\n"
                     "\t\tbase       %p\n"
                     "\t\tconn       %p\n"
                     "\t\treq        %p\n"), 
            t->thread_id, 
            t->connection_id,
            t->request_id,
            t->song_id,
            t->started,
            t->size,
            t->timer,
            t->base,
            t->conn,
            t->conn ? t->conn->request : NULL
           );
}


int rand_lim(int limit) {
/* return a uniform random integer in the range [0..limit)
 * just using mod operator can bias the output so its not
 * truly uniform ... this method does not
 */

    int divisor = RAND_MAX/limit;
    int retval;

    do { 
        retval = rand() / divisor;
    } while (retval == limit);

    return retval;
}

double rand_uniform() {
    // generates uniform random on interval [0, 1)
    return (double)rand() / ((double)RAND_MAX + 1);
}

double rand_exponential(double lambda) {
    if (lambda == 0) return 0;
    double u = rand_uniform();
    return -log(u)/lambda;
}

double rand_normal(double mean, double std) {

    // BOX-MUELLER transformation to generate two gaussian from two uniform
    // since we only need one at a time, save the other one generated
    _Thread_local static double z1 = 0;
    _Thread_local static int generate = 0;
    
    generate = !generate;
    if (!generate) 
        return z1 * std + mean;
    
    double u1, u2;
    do {
        u1 = rand_uniform();
        u2 = rand_uniform();
    } while (u1 <= DBL_EPSILON);
    
    double z0;
    z0 = sqrt(-2.0 * log(u1)) * cos (TWOPI * u2);
    z1 = sqrt(-2.0 * log(u1)) * sin (TWOPI * u2);
    
    return z0 * std + mean;
}

void schedule_request(test_t *t, int ms) { 
    //fprintf(stderr, "schedule_request()\n");
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = ms * 1000;
    evtimer_add(t->timer, &tv);
}

static evhtp_res request_done_cb(evhtp_request_t * req,  void *arg) {
//static void request_done_cb(evhtp_request_t * req, void * arg) {
    //fprintf(stderr, "request_done_cb()\n");
    test_t *t = (test_t *)arg;
    uint64_t size;
    gettimeofday(&(t->end), NULL); 
   
    GLOBAL_ADD(currently_active, -1);
    GLOBAL_ADD(accumulated_active, 1);

    size = evbuffer_get_length(req->buffer_in);
    t->size = size;
  //  fprintf(stderr, "    -thread %d connection %d received file %d of size %lu\n", 
  //          t->thread_id, t->connection_id, t->request_id, size);
    evbuffer_drain(req->buffer_out, -1); 
    if (1/*conf.active*/) {
        long thinktime = (long)(1000 * rand_exponential(2));
        schedule_request(t, thinktime);
    }
    return EVHTP_RES_OK;
}

static void request_cb(evhtp_request_t *req, void *arg) {
    //fprintf(stderr, "request_cb()\n");
    //test_t *t = (test_t *)arg;
    //evhtp_request_free(req);
    //t->req = NULL;
    evhtp_unset_all_hooks(&req->hooks);
    if (req->uri) {
        free(req->uri);
    }
    req->uri = NULL;
    evhtp_request_free(req);
}

static evhtp_res recv_chunk_cb(evhtp_request_t * req, evbuf_t * buf, void * arg) {

    test_t *t = (test_t *)arg;
    if (!t->started) {

        GLOBAL_ADD(currently_active, 1);
        GLOBAL_ADD(updates_since_tick, 1);
        GLOBAL_ADD(accumulated_active, currently_active);
        
        t->retries = 0;
        t->delay = 1;
        t->started = 1;
        gettimeofday(&(t->responded), NULL);
        uint64_t latency = TIMESTAMP(t->responded) - TIMESTAMP(t->start);

        GLOBAL_ADD(accumulated_latency, latency);
    }
    
    GLOBAL_ADD(bytes_since_tick, evbuffer_get_length(buf));

    //return EVHTP_RES_CONTINUE;
    return EVHTP_RES_OK;
}

//static evhtp_res conn_error_cb(evhtp_request_t * req, void * arg) {
static evhtp_res conn_error_cb(evhtp_connection_t * con, void * arg) {

    // retry a finite times, backing off each time
    test_t *t = (test_t *)arg;
    //print_test(t);
    evhtp_unset_all_hooks(&con->hooks);
    t->conn = NULL;
    if (t->retries <= MAX_RETRY) {
        t->retries++;
        t->delay *= 2;
        schedule_request(t, 1);
    } else {
        conf.active = 0;
    }

}

void *timer_thread(void *arg) {
    struct timeval prev, end;
    uint64_t acc, elapsed;
    uint64_t tick_active;
    uint64_t tick_updates;
    uint64_t tick_latency;

    gettimeofday(&end, NULL);
    double rate, latency;
    while (conf.active) {
        pthread_testcancel();
        usleep(TICK);
        prev = end;
        gettimeofday(&end, NULL); 
        elapsed = TIMESTAMP(end) - TIMESTAMP(prev);
        
        acc = __sync_fetch_and_and(&bytes_since_tick, 0);

        tick_active  = __sync_fetch_and_and(&accumulated_active, 0);
        tick_updates = __sync_fetch_and_and(&updates_since_tick, 0);
        tick_latency = __sync_fetch_and_and(&accumulated_latency, 0);

        rate = (double)acc * 1000000 / (elapsed * 1024 * 1024);
        if (tick_updates > 0)
            latency = (double)tick_latency / (1000*tick_updates);
        else latency = 0;
        
        double avg_active;
        if (tick_updates) 
            avg_active = (double)tick_active / tick_updates;
        else avg_active = 0;

        fprintf(stderr, "TICK: %8luus elapsed %8lu chunks %8lu songs %12lu bytes \tAVG: %8.2f MB/s %10.2fms latency\n", elapsed,  tick_active, tick_updates, acc, rate, latency);

    }
    fprintf(stderr, "   broke out of timer thread\n");
}

static void request_item(evutil_socket_t fd, short events, void *arg) {
    test_t *t      = (test_t *)arg;
    t->song_id     = (int)rand_normal(2000, 500);
    t->request_id += 1;
    t->started     = 0;
    t->size        = 0;
    evhtp_request_t *req;

    if (!t->conn) {
        t->conn = evhtp_connection_new(t->base, conf.host, conf.port);
        evhtp_connection_set_hook(t->conn, evhtp_hook_on_connection_fini, conn_error_cb, t);
    }
    
    req         = evhtp_request_new(request_cb, t);
    

//    fprintf(stderr, "    +thread %d connection %d request_item(%d)\n", t->thread_id, t->connection_id, t->song_id);
    char str_buf[256];
    snprintf(str_buf, 255, TEST_STR, t->song_id);

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Host", localhost, 0, 0));
    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("User-Agent", "daap-client", 0, 0));
    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Timeout", "1800", 0, 0));

    evhtp_request_set_hook(req, evhtp_hook_on_read, recv_chunk_cb, t);
    evhtp_request_set_hook(req, evhtp_hook_on_chunks_complete, request_done_cb, t);
    req->flags |= EVHTP_REQ_FLAG_KEEPALIVE;
    gettimeofday(&(t->start), NULL);
    evhtp_make_request(t->conn, req, htp_method_GET, str_buf);
}

test_t **test;

void *request_thread(void *arg) {
    app_t               *app    = (app_t *)arg;

    app->pid   = pthread_self();
    int active = 0;
    int ret;
    long thinktime;
    
    test_t *t = test[app->thread_id] = calloc(app->count, sizeof(test_t));
    evbase_t *evbase = event_base_new();

        for (int i = 0; i < app->count; i++) {
            t[i].req = NULL;
            t[i].thread_id  = app->thread_id;
            t[i].base = evbase;
            t[i].request_id = 0;
            t[i].connection_id = i;
            t[i].timer = evtimer_new(evbase, request_item, &t[i]);
            t[i].conn = NULL;
        }
        do {
            for (int i = 0; i < app->count; i++) {
                schedule_request(&t[i], 0);
            }
            ret = event_base_loop(evbase, 0); // ret = 1 if no more scheduled events in loop
            fprintf(stderr, "    !thread %d broke out of loop(%d)\n", app->thread_id, ret);
        } while (conf.active && ret == 1);
    
    fprintf(stderr, "   broke out of request thread\n");
    return NULL;
}


static void handle_signal(int sig) {
    if (sig == SIGKILL || sig == SIGTERM || sig == SIGSTOP) {
        fprintf(stderr, "terminating...\n");
        // cancelling the signal thread will cause a graceful shutdown
        pthread_cancel(signal_pid);
    }
    if (sig == SIGINT) {
        conf.active = 0;
        sleep(1);
        pthread_cancel(signal_pid);
    }
}

static void signal_cleanup(void *arg) {
    // we are terminating because of a signal
    // inform the other threads to terminate.
    fprintf(stderr, "cancelling threads...\n");
    
    pthread_cancel(timer_pid);
    pthread_cancel(main_pid);
    //pthread_cancel(scanner_pid);

    pthread_join(timer_pid, NULL);
    for (int i = 0; i < conf.threads; i++)
        pthread_cancel(pids[i]);
    for (int i = 0; i < conf.threads; i++)
        pthread_join(pids[i], NULL);
    pthread_join(main_pid, NULL);

    exit(EXIT_SUCCESS);
}


static void *signal_thread(void *arg) {
    int cleanup_pop_val;
    signal_pid = pthread_self();
    pthread_cleanup_push(signal_cleanup, NULL);
    struct sigaction act = {0};
    act.sa_handler = handle_signal;
    for (int i = 0; i < 32; i++)
        sigaction(i, &act, NULL);
    sigset_t mask;
    sigemptyset(&mask);
    int sig;
    while(conf.active) {
        sigsuspend(&mask);
    }
    pthread_cleanup_pop(cleanup_pop_val);
}

void assign_signal_handler() {
    // first create the signal handler while no signals are blocked
    pthread_create((pthread_t *)&signal_pid, NULL, &signal_thread, NULL);
    pthread_detach(signal_pid);
    // now block all signals - all subsequent threads also will block
    sigset_t mask;
    sigfillset(&mask);
    sigprocmask(SIG_SETMASK, &mask, NULL);

}

static const char option_string[]  = "Vc:l:p:t:T:h:";
static struct option long_options[] = {
    { "verbose",            no_argument,       0,       'V' },
    { "count",              required_argument, 0,       'c' },
    { "limit",              required_argument, 0,       'l' },
    { "port",               required_argument, 0,       'p' },
    { "threads",            required_argument, 0,       't' },
    { "timeout",            required_argument, 0,       'T' },
    { "host",               required_argument, 0,       'h' },
    { 0, 0, 0, 0 }
};

#define INTARG(a, desc) {                                               \
      a = atoi(optarg);                                                 \
      if (a == 0) {                                                     \
          fprintf(stderr, "--%s must be passed an integer.\n", desc);   \
          exit(1);                                                      \
      }                                                                 \
}


int main(int argc, char ** argv) {
// process cmdline args

    conf.host = "127.0.0.1";
    conf.port = 3689;
    conf.timeout = 1800;
    conf.limit = 3000;
    conf.threads = 1;
    conf.count = 4;
	conf.mac   = mac_address();

    struct timeval now;
    gettimeofday(&now, NULL);
    uint64_t seed = conf.mac + TIMESTAMP(now);
    srand((unsigned)seed);
    while(1) {
        int option_index = 0;
        int c = getopt_long(argc, argv, option_string, long_options, &option_index);
        if (c == -1) break;
        switch(c) {
            case 'V': conf.verbose = 1;
                      break;
            case 'c': INTARG(conf.count, "count");
                      break;
            case 'l': INTARG(conf.limit, "limit");
                      break;
            case 'p': INTARG(conf.port, "port");
                      break;
            case 't': INTARG(conf.threads, "threads");
                      break;
            case 'T': INTARG(conf.timeout, "timeout");
                      break;
            case 'h': {
                          conf.host = strdup(optarg);
                      }
                      break;
            default:
                      exit(1);
        }
    }

	gethostname(localhost, 256);

    conf.active = 1;
    assign_signal_handler();


    pids = calloc(conf.threads, sizeof(pthread_t));
    apps = calloc(conf.threads, sizeof(app_t));
    test = calloc(conf.threads, sizeof(test_t *));
        pthread_create(&timer_pid, NULL, timer_thread, NULL);

        for (int i = 0; i < conf.threads; i++) {
            apps[i].thread_id = i;
            apps[i].count     = conf.count;
            pthread_create((pthread_t *)&pids[i], NULL, request_thread, &apps[i]);
        }
        
        for (int i = 0; i < conf.threads; i++) {
           pthread_join(pids[i], NULL);
        } 
        free(apps);
        free(pids);
    fprintf(stderr, "main thread terminated\n");
}
