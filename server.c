#include <stdio.h>
#include <errno.h>

#if defined (DEBUG_SYSLOG)
  #include <syslog.h>
  #define DPRINT_DEBUG LOG_DEBUG
  #define DPRINT_ERROR LOG_ERR
  #define DPRINT(priority, text, args...) syslog(priority, text, ##args)
#elif defined (DEBUG_CONSOLE)
  #define DPRINT_DEBUG stdout
  #define DPRINT_ERROR stderr
  #define DPRINT(file, text, args...) fprintf(file, text, ##args)
#else
  #define DPRINT_DEBUG
  #define DPRINT_ERROR
  #define DPRINT(x,y,z...)
#endif

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <sys/time.h>
#include <event.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>


#define MAX_CONNECTIONS 100
#define BUFFER_SIZE 256
#define INTERVAL 1

struct _event_group;

typedef struct _event_data_wrap {
  int fd;
  int buf_sz;
  short eflags;
  void *params;
  struct timeval *tv;
  struct event event;
  struct _event_group *group;
  void (*callback)(int, short, void *arg);
  struct sockaddr_storage peer_s;
} event_data_wrap; 


typedef struct _stats {
  unsigned long total;
  unsigned long current;
  pthread_mutex_t lock;
} stats;


typedef struct _event_group {
    struct event_base *b;
    int max;
    int cur;
    stats stats;
    event_data_wrap *events[];
} event_group;


typedef struct _params
{
  char *ip;
  int port;
  int is_daemon;
  int stype;
  int buf_sz;
  int max_cons;
  unsigned int interval;
} params;


typedef struct _run_data
{
    int s;
    event_group *e_group;
    params p;
    struct sockaddr_storage saddr_s;
} run_data;


int update_stats(stats *stats_p, unsigned long val)
{
    int ret = 1;

    if(!pthread_mutex_lock(&stats_p->lock)) {
        stats_p->total += val;
        stats_p->current += val;
        pthread_mutex_unlock(&stats_p->lock);
        ret = 0;
    }

    return (ret);
}


int setup_event(event_data_wrap *e_wrap)
{
    /* TODO: add error checking in case something goes wrong... */
    event_set(&e_wrap->event, e_wrap->fd, e_wrap->eflags, e_wrap->callback, 
        e_wrap->params);
    event_base_set(e_wrap->group->b, &e_wrap->event);
    event_add(&e_wrap->event, e_wrap->tv);

    return (0);
}


int add_to_group(event_data_wrap *e_wrap)
{
    if(e_wrap->group != NULL && e_wrap->group->cur < e_wrap->group->max) {
        e_wrap->group->events[e_wrap->group->cur] = e_wrap;
        ++e_wrap->group->cur;
    }
    else {
        return (-1);
    }

    return (0);
}


int destroy_event(event_data_wrap *e_wrap)
{
    event_del(&e_wrap->event);
    --e_wrap->group->cur;

    if(e_wrap->tv != NULL) {
        free(e_wrap->tv);
    }

    free(e_wrap);

    return (0);
}


int setup_event_group(event_group **grp, int max)
{
   size_t size;
   
   size = sizeof(event_group) + (max * sizeof(event_data_wrap *));
   *grp = (event_group *) calloc(1, size);
   if((*grp) != NULL) {
       (*grp)->cur = 0;
       (*grp)->max = max;

       (*grp)->stats.total = 0;
       if(pthread_mutex_init(&(*grp)->stats.lock, NULL)) {
          DPRINT(DPRINT_ERROR, "[%s] unable to initialize mutex\n",
              __FUNCTION__);
          return (-1);
       }

       (*grp)->b = event_base_new();
       if((*grp)->b == NULL) {
          pthread_mutex_destroy(&(*grp)->stats.lock);
          DPRINT(DPRINT_ERROR, "[%s] libevent error\n", __FUNCTION__);
          return (-1);
       }
   }
   else {
       DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
       return (-1);
   }

   return (0);
}


int destroy_event_group(event_group **grp)
{
    int i, max;

    pthread_mutex_destroy(&(*grp)->stats.lock);
    
    max = (*grp)->cur;
    for(i = 0; i < max; ++i) {
        destroy_event((*grp)->events[i]);
    }

    event_base_free((*grp)->b);
    free(*grp);

    return (0);
}


void recv_data_tcp(int fd, short event, void *arg)
{
    int recv_sz = 0;
    char *recv_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    
    recv_buff = (char *) malloc(e_wrap->buf_sz);
    /* if malloc fails, WE.ARE.SCREWED */

    memset(recv_buff, '\0', sizeof(recv_buff));
    recv_sz = recv(fd, (void *)recv_buff, e_wrap->buf_sz, 0); 
    if(recv_sz > 0) {
        update_stats(&e_wrap->group->stats, recv_sz);
    }
    else {
        DPRINT(DPRINT_DEBUG, "[%s] closing socket [%d]\n", __FUNCTION__, fd);
        close(fd);
        destroy_event(e_wrap);
    }
 
    free(recv_buff);
}


void output_stats(int fd, short event, void *arg)
{
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    stats *stats_p = &e_wrap->group->stats; 
    unsigned long t = 0;
    unsigned long c = 0;

    if(!pthread_mutex_lock(&stats_p->lock)) {
        c = stats_p->current;
        stats_p->current = 0;

        /* assumption: if current is 0 no traffic is coming in */
        /* in this case we reset the total counter             */
        if(c == 0) {
            t = 0;
            stats_p->total = 0;
        }
        else {
            t = stats_p->total;
        }

        pthread_mutex_unlock(&stats_p->lock);

        DPRINT(DPRINT_DEBUG, "[%s] total [%ld] current[%ld]\n", 
           __FUNCTION__, t, c);
    }

    event_add(&e_wrap->event, e_wrap->tv);
}


void recv_data_udp(int fd, short event, void *arg)
{
    int recv_sz = 0;
    socklen_t sz;
    char *recv_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;

    recv_buff = (char *) malloc(e_wrap->buf_sz);
    /* if malloc fails, WE.ARE.SCREWED */
    
    memset(recv_buff, '\0', sizeof(recv_buff));
    sz = sizeof(struct sockaddr);
    recv_sz = recvfrom(fd, (void *)recv_buff, e_wrap->buf_sz, 0,
        (struct sockaddr *)&e_wrap->peer_s, &sz); 
    if(recv_sz > 0) {
        update_stats(&e_wrap->group->stats, recv_sz);
    }

    free(recv_buff);
}


void cons_read(int fd, short event, void *arg)
{
    int recv_sz = 0;
    char read_buff[256];
    struct event_base *b = (struct event_base *)arg;
    
    recv_sz = read(fd, (void *)read_buff, sizeof(read_buff));
    if(recv_sz > 0) {
        if(recv_sz == 1 && read_buff[0] == '\n') {
            event_base_loopbreak(b);
        }
    }
}


void handle_signal(int fd, short event, void *arg)
{
    struct event_base *b = (struct event_base *)arg;
    event_base_loopbreak(b);
}


void accept_conn(int fd, short event, void *arg)
{
    int new_conn;
    socklen_t sz;
    event_data_wrap *recv_event = NULL;
    run_data *rd = (run_data *)arg;
    struct sockaddr_storage peer;
    
    sz = sizeof(struct sockaddr);
    memset(&peer, 0, sizeof(struct sockaddr_storage));
    new_conn = accept(fd, (struct sockaddr *)&peer, &sz);
    if(new_conn > 0) {
        recv_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(recv_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
        }
        else {
            recv_event->fd = new_conn;
            recv_event->eflags = (EV_READ | EV_PERSIST);
            recv_event->group = rd->e_group;
            recv_event->callback = recv_data_tcp;
            recv_event->tv = NULL;
            recv_event->params = recv_event;
            recv_event->buf_sz = rd->p.buf_sz;
            memcpy(&recv_event->peer_s, &peer, 
                sizeof(struct sockaddr_storage));

            if(setup_event(recv_event) < 0 || add_to_group(recv_event) < 0) {
                    DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", 
                    __FUNCTION__);

                close(new_conn);
                free(recv_event);
            }
            else {
                DPRINT(DPRINT_DEBUG, "[%s] connection accepted socket [%d]\n", 
                    __FUNCTION__, new_conn);
            }
         }
    }
}


int loop_tcp(run_data *rd)
{
    /* note: all event_data_wrap allocated inside this function will be      */
    /*       free()'d by run() during destroy_event_group(). in an event     */
    /*       where a call to setup_event() or add_to_group() fails, free()   */
    /*       is called immediately since destroy_event_group() are not aware */
    /*       of them and won't be able to free() them                        */

    event_data_wrap *output_event = NULL;
    event_data_wrap *accept_event = NULL;
    event_data_wrap *console_event = NULL;
    event_data_wrap *signal_event = NULL;

    DPRINT(DPRINT_DEBUG, "[%s] starting...\n", __FUNCTION__);

    accept_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
    if(accept_event == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
        return (1);
    }

    accept_event->fd = rd->s;
    accept_event->eflags = (EV_READ | EV_PERSIST);
    accept_event->group = rd->e_group;
    accept_event->callback = accept_conn;
    accept_event->tv = NULL;
    accept_event->params = rd;

    if(setup_event(accept_event) < 0 || add_to_group(accept_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
        free(accept_event);
        return (1);
    }

    if(!rd->p.is_daemon) {
        console_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(console_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
            return (1);
        }

        console_event->fd = STDIN_FILENO;
        console_event->eflags = (EV_READ | EV_PERSIST);
        console_event->group = rd->e_group;
        console_event->callback = cons_read;
        console_event->tv = NULL;
        console_event->params = rd->e_group->b;

        if(setup_event(console_event) < 0 || add_to_group(console_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
            free(console_event);
            return (1);
        }

        output_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(output_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
            return (1);
        }

        output_event->fd = -1;
        output_event->eflags = 0;
        output_event->group = rd->e_group;
        output_event->callback = output_stats;

        output_event->tv = (struct timeval *) calloc(1, sizeof(struct timeval));
        output_event->tv->tv_usec = 0;
        output_event->tv->tv_sec = rd->p.interval;

        output_event->params = output_event;

        if(setup_event(output_event) < 0 || add_to_group(output_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
            free(output_event);
            return (1);
        }
    }

    signal_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
    if(signal_event == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
        return (1);
    }

    signal_event->fd = SIGTERM;
    signal_event->eflags = (EV_SIGNAL | EV_PERSIST);
    signal_event->group = rd->e_group;
    signal_event->callback = handle_signal;

    signal_event->tv = NULL;
    signal_event->params = rd->e_group->b;

    if(setup_event(signal_event) < 0 || add_to_group(signal_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
        free(signal_event);
        return (1);
    }

    if(listen(rd->s, 5) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] listen() failed\n", __FUNCTION__);
        return (1);
    }

    event_base_dispatch(rd->e_group->b);

    DPRINT(DPRINT_DEBUG, "[%s] exiting...\n", __FUNCTION__);

    return (0);
}


int loop_udp(run_data *rd)
{
    /* note: all event_data_wrap allocated inside this function will be      */
    /*       free()'d by run() during destroy_event_group(). in an event     */
    /*       where a call to setup_event() or add_to_group() fails, free()   */
    /*       is called immediately since destroy_event_group() are not aware */
    /*       of them and won't be able to free() them                        */

    event_data_wrap *read_event = NULL;
    event_data_wrap *console_event = NULL;
    event_data_wrap *output_event = NULL;
    event_data_wrap *signal_event = NULL;

    DPRINT(DPRINT_DEBUG, "[%s] starting...\n", __FUNCTION__);

    read_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
    if(read_event == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
        return (1);
    }

    read_event->fd = rd->s;
    read_event->eflags = (EV_READ | EV_PERSIST);
    read_event->group = rd->e_group;
    read_event->callback = recv_data_udp;
    read_event->tv = NULL;
    read_event->buf_sz = rd->p.buf_sz;
    read_event->params = read_event;

    if(setup_event(read_event) < 0 || add_to_group(read_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
        free(read_event);
        return (1);
    }

    /* we only enable the following events if we're not running as a daemon */
    if(!rd->p.is_daemon) {
        console_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(console_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
            return (1);
        }

        console_event->fd = STDIN_FILENO;
        console_event->eflags = (EV_READ | EV_PERSIST);
        console_event->group = rd->e_group;
        console_event->callback = cons_read;
        console_event->tv = NULL;
        console_event->params = rd->e_group->b;

        if(setup_event(console_event) < 0 || add_to_group(console_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
            free(console_event);
            return (1);
        }

        output_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(output_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
            return (1);
        }

        output_event->fd = -1;
        output_event->eflags = 0;
        output_event->group = rd->e_group;
        output_event->callback = output_stats;

        output_event->tv = (struct timeval *) calloc(1, sizeof(struct timeval));
        output_event->tv->tv_usec = 0;
        output_event->tv->tv_sec = rd->p.interval;

        output_event->params = output_event;

        if(setup_event(output_event) < 0 || add_to_group(output_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
            free(output_event);
            return (1);
        }
    }

    signal_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
    if(signal_event == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
        return (1);
    }

    signal_event->fd = SIGTERM;
    signal_event->eflags = (EV_SIGNAL | EV_PERSIST);
    signal_event->group = rd->e_group;
    signal_event->callback = handle_signal;

    signal_event->tv = NULL;
    signal_event->params = rd->e_group->b;

    if(setup_event(signal_event) < 0 || add_to_group(signal_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
        free(signal_event);
        return (1);
    }

    event_base_dispatch(rd->e_group->b);

    DPRINT(DPRINT_DEBUG, "[%s] exiting...\n", __FUNCTION__);

    return (0);
}


int run(run_data *rd)
{
    DPRINT(DPRINT_DEBUG, "[%s] starting...\n", __FUNCTION__);

    rd->s = socket(rd->saddr_s.ss_family, rd->p.stype, 0);
    if(rd->s < 0) {
        DPRINT(DPRINT_ERROR, "[%s] socket() failed\n", __FUNCTION__);
        return (1);
    }
  
    if(bind(rd->s, (struct sockaddr *)&rd->saddr_s, 
        sizeof(struct sockaddr)) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] bind() failed\n", __FUNCTION__);
            close(rd->s);
            return (1);
    }

    if(setup_event_group(&rd->e_group, rd->p.max_cons) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event groups\n", 
            __FUNCTION__);
        close(rd->s);
        return (1);
    }

    DPRINT(DPRINT_DEBUG, "[%s] libevent using [%s]\n", __FUNCTION__,
        event_base_get_method(rd->e_group->b));

    if(rd->p.stype == SOCK_STREAM) {
        loop_tcp(rd);
    }
    else if(rd->p.stype == SOCK_DGRAM) {
        loop_udp(rd);
    }

    destroy_event_group(&rd->e_group);

    close(rd->s);

    DPRINT(DPRINT_DEBUG, "[%s] exiting...\n", __FUNCTION__);

    return (0);
}


int run4(run_data *rd)
{
    socklen_t sz;
    struct sockaddr_in si;

    DPRINT(DPRINT_DEBUG, "[%s] starting...\n", __FUNCTION__);

    memset(&si, 0, sizeof(struct sockaddr_in));
    sz = sizeof(struct sockaddr_in);
    si.sin_family = AF_INET;
    si.sin_port = htons(rd->p.port);
    if(inet_pton(AF_INET, rd->p.ip, (void *)&si.sin_addr) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] inet_pton() failed\n", __FUNCTION__);
        return (1);
    }

    memcpy(&rd->saddr_s, &si, sizeof(struct sockaddr_storage));

    run(rd);
    DPRINT(DPRINT_DEBUG, "[%s] exiting...\n", __FUNCTION__);

    return (0);
}


int run6(run_data *rd)
{
    socklen_t sz;
    struct sockaddr_in6 si;

    DPRINT(DPRINT_DEBUG, "[%s] starting...\n", __FUNCTION__);

    memset(&si, 0, sizeof(struct sockaddr_in6));
    sz = sizeof(struct sockaddr_in6);
    si.sin6_family = AF_INET6;
    si.sin6_port = htons(rd->p.port);
    if(inet_pton(AF_INET6, rd->p.ip, (void *)&si.sin6_addr) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] inet_pton() failed\n", __FUNCTION__);
        return (1);
    }
  
    memcpy(&rd->saddr_s, &si, sizeof(struct sockaddr_storage));

    run(rd);
    DPRINT(DPRINT_DEBUG, "[%s] exiting...\n", __FUNCTION__);

    return (0);
}


void print_usage(char *cmd)
{
    DPRINT(DPRINT_ERROR,"Usage: %s [parameters]\n", cmd);
    DPRINT(DPRINT_ERROR,"Required parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[%s]\n", 
        "-4 <IPv4 address> | -6 <IPv6 address>");
    DPRINT(DPRINT_ERROR,"\t[-p <port number>]\n");
    DPRINT(DPRINT_ERROR,"\t[-t <protocol (tcp|udp)>]\n");
    DPRINT(DPRINT_ERROR,"Optional parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[-S <size of data>]\n");
    DPRINT(DPRINT_ERROR,"\t[-i <interval (seconds) for displaying stats>]\n");
    DPRINT(DPRINT_ERROR,"\t[-d <run as daemon>]\n");
    DPRINT(DPRINT_ERROR,"Defaults:\n");
    DPRINT(DPRINT_ERROR,"\tSize of receive data: 256 bytes\n");
    DPRINT(DPRINT_ERROR,"\tDisplay status interval: 1 second\n");
}


int main(int argc, char *argv[])
{
    int ipver = 0;
    int len = 0;
    int opt;

    params p = {NULL, 0, 0, 0, BUFFER_SIZE, MAX_CONNECTIONS, INTERVAL};
    run_data rd;

    while((opt = getopt(argc, argv, "4:6:p:t:S:i:m:d")) != -1) {
        switch(opt) {
          case '4':
              p.ip = argv[optind - 1];
              ipver = 4;
              break;

          case '6':
              p.ip = argv[optind - 1];
              ipver = 6;
              break;

          case 'p':
              p.port = (int) strtol(optarg, (char **)NULL, 10);
              break;

          case 't':
              len = strlen(argv[optind - 1]);

              if(!strncmp(argv[optind - 1], "tcp", len))
                  p.stype = SOCK_STREAM;
              else if(!strncmp(argv[optind - 1], "udp", len))
                  p.stype = SOCK_DGRAM;

              break;

          case 'S':
              p.buf_sz = (int) strtol(optarg, (char **)NULL, 10);
              break;

          case 'i':
              p.interval = (int) strtol(optarg, (char **)NULL, 10);
              break;

          case 'm':
              p.max_cons = (int) strtol(optarg, (char **)NULL, 10);
              break;

          case 'd':
              p.is_daemon = 1;
              break;

          case 'h':
              print_usage(argv[0]);
              return (1);
              break;

          default:
              print_usage(argv[0]);
              return (1);
              break;
        }
    }

    if(p.ip == NULL || p.port == 0 || p.stype == 0) {
        DPRINT(DPRINT_ERROR,"ERROR: Parameters are not valid\n");
        print_usage(argv[0]);
        return (1);
    }

    /* always add three for the default three events that we expect other */
    /* than socket receive events: listen socket events, console events,  */
    /* stats timer output events                                          */
    p.max_cons += 3;

    memset(&rd, 0, sizeof(run_data));
    memcpy(&rd.p, &p, sizeof(params));

    if(rd.p.is_daemon) {
        if(daemon(0,0) < 0) {
            DPRINT(DPRINT_ERROR,"ERROR: unable to run as daemon\n");
            return (1);
        }
    }

    switch(ipver) {
        case 4:
            run4(&rd);
            break;

        case 6:
            run6(&rd);
            break;

        default:
            break;
    }

    return (0);
}
