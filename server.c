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
#include <sys/queue.h>

#include <pthread.h>


#define MAX_CONNECTIONS 100
#define BUFFER_SIZE 256
#define INTERVAL 1

#define SRV_R_EVENT 0
#define SRV_W_EVENT 1

struct _event_group;

struct echo_node {
    size_t echo_send_sz;
    TAILQ_ENTRY(echo_node) entries;
};

typedef struct _echo_queue {
    int active;
    void *owner;
    u_int32_t count;
    pthread_mutex_t lock;
    TAILQ_HEAD(echo_queue_h, echo_node) head;
} echo_queue;


typedef struct _event_data_wrap {
    int fd;
    int buf_sz;
    short eflags;
    void *params;
    struct timeval *tv;
    struct event event;
    struct _event_data_wrap **group_bp;
    struct _event_group *group;
    void (*callback)(int, short, void *arg);
    struct sockaddr_storage peer_s;
    size_t peer_sz; /* used for udp */
    echo_queue *eq;
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
  int do_echo;
} params;


typedef struct _run_data
{
    int s;
    event_group *e_group;
    params p;
    struct sockaddr_storage saddr_s;
    size_t saddr_sz;
} run_data;


/* this function is redundant with client.c, needs merging */
int srv_is_val_set(int base, int val, pthread_mutex_t *l)
{
    int ret;

    ret = pthread_mutex_trylock(l);
    if(ret == 0) {
        ret = (base == val) ? 1 : 0;
        pthread_mutex_unlock(l);    
        return (ret);
    }

    return (0);
}


/* this function is redundant with client.c, needs merging */
int srv_set_val(int *base, int val, pthread_mutex_t *l)
{
    int ret;

    /* let's not waste locking, if the value is already set then return */
    if(*base == val)
        return (1);

    ret = pthread_mutex_trylock(l);
    if(ret == 0) {
        *base = val;
        pthread_mutex_unlock(l);    
        return (1);
    }
    
    return (0);
}


/* this function is redundant with client.c, needs merging */
int srv_inc_val(int *val, pthread_mutex_t *l)
{
    int ret;

    ret = pthread_mutex_trylock(l);
    if(ret == 0) {
        *val = (*val) + 1;
        pthread_mutex_unlock(l);    
        return (1);
    }
    
    return (0);
}


/* this function is redundant with client.c, needs merging */
void srv_sleep_random(void)
{
    struct timeval tv;

#if defined (__linux__)
    struct drand48_data buff;
    long int res;
    unsigned int t;
#endif

#if defined (__linux__)
    gettimeofday(&tv, NULL);
    srand48_r(tv.tv_usec, &buff);
    lrand48_r(&buff, &res);
    t = ((res >> 8) & (1000000));
    usleep(t);
#elif defined (__FreeBSD__)
    gettimeofday(&tv, NULL);
    usleep((arc4random() % (tv.tv_usec + 1)) / 100);
#else
    gettimeofday(&tv, NULL);
    srand((tv.tv_sec + tv.tv_usec) >> (sizeof(long) / 2));
    usleep((rand() & 1000000) >> 2);
#endif
}


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
    event_set(&e_wrap->event, e_wrap->fd, e_wrap->eflags, e_wrap->callback, 
        e_wrap->params);
    event_base_set(e_wrap->group->b, &e_wrap->event);
    return(event_add(&e_wrap->event, e_wrap->tv));
}


int add_to_group(event_data_wrap *e_wrap)
{
    if(e_wrap->group != NULL && e_wrap->group->cur < e_wrap->group->max) {
        e_wrap->group->events[e_wrap->group->cur] = e_wrap;
        e_wrap->group_bp = &e_wrap->group->events[e_wrap->group->cur];
        ++e_wrap->group->cur;
    }
    else {
        return (-1);
    }

    return (0);
}


int destroy_event(event_data_wrap *e_wrap)
{
    struct echo_node *en_p = NULL;

    event_del(&e_wrap->event);

    if(e_wrap->eq != NULL && e_wrap->eq->owner == e_wrap) {
        while(pthread_mutex_trylock(&e_wrap->eq->lock) != 0) 
            srv_sleep_random();

         e_wrap->eq->active = 0;
         while(!TAILQ_EMPTY(&e_wrap->eq->head)) {
             en_p = TAILQ_FIRST(&e_wrap->eq->head);
             TAILQ_REMOVE(&e_wrap->eq->head, en_p, entries);
             free(en_p);
         }

         pthread_mutex_unlock(&e_wrap->eq->lock);
         pthread_mutex_destroy(&e_wrap->eq->lock);
         free(e_wrap->eq);
    }

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
        if((*grp)->events[i] != NULL)
            destroy_event((*grp)->events[i]);
    }

    event_base_free((*grp)->b);
    free(*grp);

    return (0);
}


int config_event(event_data_wrap *e)
{
    if(add_to_group(e) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to add event\n", __FUNCTION__);
        return (-1);
    }
    else if(setup_event(e) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to setup event\n", __FUNCTION__);
        (*e->group_bp) = NULL;
        return (-1);
    }

    return (0);
}


void r_data_tcp(int fd, void *arg)
{
    size_t recv_sz = 0;
    char *recv_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    struct echo_node *en_p = NULL;

    recv_buff = (char *) malloc(e_wrap->buf_sz);
    /* if malloc fails, WE.ARE.SCREWED */

    memset(recv_buff, '\0', sizeof(recv_buff));
    recv_sz = recv(fd, (void *)recv_buff, e_wrap->buf_sz, 0); 
    if(recv_sz > 0) {
#if defined (__amd64__)
        DPRINT(DPRINT_DEBUG, "[%s] received %ld bytes\n", 
            __FUNCTION__, recv_sz);
#elif defined (__i386__)
        DPRINT(DPRINT_DEBUG, "[%s] received %d bytes\n", 
            __FUNCTION__, recv_sz);
#endif

        update_stats(&e_wrap->group->stats, recv_sz);
  
        if(e_wrap->eq != NULL &&
            srv_is_val_set(e_wrap->eq->active, 1, &e_wrap->eq->lock)) {

            en_p = (struct echo_node *) malloc(sizeof(struct echo_node));

            while(pthread_mutex_trylock(&e_wrap->eq->lock) != 0) 
                srv_sleep_random();

            en_p->echo_send_sz = recv_sz;
            TAILQ_INSERT_TAIL(&e_wrap->eq->head, en_p, entries);

            pthread_mutex_unlock(&e_wrap->eq->lock);
        }
    }
    else {
        DPRINT(DPRINT_DEBUG, "[%s] closing socket [%d], error [%d]\n", 
            __FUNCTION__, fd, ((recv_sz < 0) ? errno : 0));
        close(fd);
        destroy_event(e_wrap);
    }
 
    free(recv_buff);
}


void w_data_tcp(int fd, void *arg)
{
    size_t send_sz = 0;
    size_t sent = 0;
    char *send_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    struct echo_node *en_p = NULL;
    
    if(srv_is_val_set(e_wrap->eq->active, 1, &e_wrap->eq->lock)) {
        while(pthread_mutex_trylock(&e_wrap->eq->lock) != 0) 
            srv_sleep_random();

        if(!TAILQ_EMPTY(&e_wrap->eq->head)) {
            en_p = TAILQ_FIRST(&e_wrap->eq->head);
            send_sz = en_p->echo_send_sz;
            TAILQ_REMOVE(&e_wrap->eq->head, en_p, entries);
            free(en_p);
        } 

        pthread_mutex_unlock(&e_wrap->eq->lock);

        if(send_sz > 0)
        {
            send_buff = (char *) malloc(send_sz);
            /* if malloc fails, WE.ARE.SCREWED */

            if(send_buff != NULL) {
               sent = send(fd, send_buff, send_sz, 0);
#if defined (__amd64__)
               DPRINT(DPRINT_DEBUG, "[%s] sent %ld bytes\n", __FUNCTION__,
                   sent);
#elif defined (__i386__)
               DPRINT(DPRINT_DEBUG, "[%s] sent %d bytes\n", __FUNCTION__,
                   sent);
#endif
               free(send_buff);
            }
        }
    }
}


void rw_data_tcp(int fd, short event, void *arg)
{
    if(event == EV_READ)
        r_data_tcp(fd, arg);
    
    if(event == EV_WRITE)
        w_data_tcp(fd, arg);
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
   
        /*DPRINT(DPRINT_DEBUG, "[total: %ld current: %ld]\n", t, c);*/
    }

    event_add(&e_wrap->event, e_wrap->tv);
}


void w_data_udp(int fd, short event, void *arg)
{
    size_t send_sz = 0;
    size_t sent = 0;
    char *send_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    struct echo_node *en_p = NULL;
    
    if(srv_is_val_set(e_wrap->eq->active, 1, &e_wrap->eq->lock)) {
        while(pthread_mutex_trylock(&e_wrap->eq->lock) != 0) 
            srv_sleep_random();

        if(!TAILQ_EMPTY(&e_wrap->eq->head)) {
            en_p = TAILQ_FIRST(&e_wrap->eq->head);
            send_sz = en_p->echo_send_sz;
            TAILQ_REMOVE(&e_wrap->eq->head, en_p, entries);
            free(en_p);
        } 

        pthread_mutex_unlock(&e_wrap->eq->lock);

        if(send_sz > 0)
        {
            send_buff = (char *) malloc(send_sz);
            /* if malloc fails, WE.ARE.SCREWED */

            if(send_buff != NULL) {
               sent = sendto(fd, send_buff, send_sz, 0,
                          (struct sockaddr *)&e_wrap->peer_s, e_wrap->peer_sz);
#if defined (__amd64__)
               DPRINT(DPRINT_DEBUG, "[%s] sent %ld bytes\n", __FUNCTION__,
                   sent);
#elif defined (__i386__)
               DPRINT(DPRINT_DEBUG, "[%s] sent %d bytes\n", __FUNCTION__,
                   sent);
#endif
               free(send_buff);
            }
        }
    }
}


void r_data_udp(int fd, short event, void *arg)
{
    size_t recv_sz = 0;
    char *recv_buff = NULL;
    event_data_wrap *e_wrap = (event_data_wrap *)arg;
    struct echo_node *en_p = NULL;

    recv_buff = (char *) malloc(e_wrap->buf_sz);
    /* if malloc fails, WE.ARE.SCREWED */
    
    memset(recv_buff, '\0', sizeof(recv_buff));
    recv_sz = recvfrom(fd, (void *)recv_buff, e_wrap->buf_sz, 0,
        (struct sockaddr *)&e_wrap->peer_s, (socklen_t *)&e_wrap->peer_sz); 
    if(recv_sz > 0) {
#if defined (__amd64__)
        DPRINT(DPRINT_DEBUG, "[%s] received %ld bytes\n", 
            __FUNCTION__, recv_sz);
#elif defined (__i386__)
        DPRINT(DPRINT_DEBUG, "[%s] received %d bytes\n", 
            __FUNCTION__, recv_sz);
#endif

        update_stats(&e_wrap->group->stats, recv_sz);
  
        if(e_wrap->eq != NULL &&
            srv_is_val_set(e_wrap->eq->active, 1, &e_wrap->eq->lock)) {

            en_p = (struct echo_node *) malloc(sizeof(struct echo_node));

            while(pthread_mutex_trylock(&e_wrap->eq->lock) != 0) 
                srv_sleep_random();

            en_p->echo_send_sz = recv_sz;
            TAILQ_INSERT_TAIL(&e_wrap->eq->head, en_p, entries);

            pthread_mutex_unlock(&e_wrap->eq->lock);
        }
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
    event_data_wrap *r_event = NULL;
    event_data_wrap *w_event = NULL;
    run_data *rd = (run_data *)arg;
    struct sockaddr_storage peer;
    
    sz = sizeof(struct sockaddr);
    memset(&peer, 0, sizeof(struct sockaddr_storage));
    new_conn = accept(fd, (struct sockaddr *)&peer, &sz);
    if(new_conn > 0) {
        DPRINT(DPRINT_DEBUG, "[%s] connection accepted, socket [%d]\n",
            __FUNCTION__, new_conn);

        r_event = (event_data_wrap *) malloc(sizeof(event_data_wrap));

        r_event->fd = new_conn;
        r_event->eflags = (EV_READ | EV_PERSIST);
        r_event->group = rd->e_group;
        r_event->callback = rw_data_tcp;
        r_event->tv = NULL;
        r_event->params = r_event;
        r_event->buf_sz = rd->p.buf_sz;
        r_event->eq = NULL;
        r_event->group_bp = NULL;
        memcpy(&r_event->peer_s, &peer, 
            sizeof(struct sockaddr_storage));

        if(config_event(r_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
            free(r_event);
            r_event = NULL;
        }

        if(r_event != NULL && rd->p.do_echo) {
            w_event = (event_data_wrap *) malloc(sizeof(event_data_wrap));

            memcpy(w_event, r_event,
                sizeof(event_data_wrap));

            w_event->eflags = (EV_WRITE | EV_PERSIST);
            w_event->params = w_event;

            w_event->eq = (echo_queue *) malloc(sizeof(echo_queue));
            memset(w_event->eq, 0, sizeof(echo_queue));
            pthread_mutex_init(&w_event->eq->lock, NULL);
            TAILQ_INIT(&w_event->eq->head);

            r_event->eq = w_event->eq;
            /* XXX this is evil... */
            w_event->eq->owner = (void *)w_event->eq;

            if(config_event(w_event) < 0) {
                DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                    __FUNCTION__);
                free(w_event);
                w_event = NULL;

                *r_event->group_bp = NULL;
                destroy_event(r_event);
                free(r_event);
                r_event = NULL;
            }
            else {
                while(srv_set_val(&w_event->eq->active, 1,
                    &w_event->eq->lock) != 1)
                        srv_sleep_random();
            }
        }

       if(r_event == NULL && w_event == NULL) {
           close(new_conn);
           DPRINT(DPRINT_DEBUG, "[%s] closing [%d], unable to setup event\n",
               __FUNCTION__, new_conn);
       }
    }
}


int loop_tcp(run_data *rd)
{
    /* note: all event_data_wrap allocated inside this function will be      */
    /*       free()'d by run() during destroy_event_group(). in an event     */
    /*       where a call to config_event() fails, free() should be called   */
    /*       immediately since destroy_event_group() is not aware of those   */
    /*       failed events                                                   */

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
    accept_event->group_bp = NULL;

    if(config_event(accept_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n", __FUNCTION__);
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
        console_event->group_bp = NULL;

        if(config_event(console_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
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

        output_event->tv =
            (struct timeval *) calloc(1,sizeof(struct timeval));
        output_event->tv->tv_usec = 0;
        output_event->tv->tv_sec = rd->p.interval;

        output_event->params = output_event;
        output_event->group_bp = NULL;

        if(config_event(output_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
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
    signal_event->group_bp = NULL;

    if(config_event(signal_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n", __FUNCTION__);
        free(output_event);
        return (1);
    }

    /* is 5 enough? */
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
    /*       where a call to config_event() fails, free() should be called   */
    /*       immediately since destroy_event_group() is not aware of those   */
    /*       failed events                                                   */

    event_data_wrap *read_event = NULL;
    event_data_wrap *send_event = NULL;
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
    read_event->callback = r_data_udp;
    read_event->tv = NULL;
    read_event->buf_sz = rd->p.buf_sz;
    read_event->params = read_event;
    read_event->group_bp = NULL;
    read_event->peer_sz = rd->saddr_sz;

    if(config_event(read_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n", __FUNCTION__);
        free(read_event);
        return (1);
    }

    if(rd->p.do_echo) {
        send_event = (event_data_wrap *) calloc(1, sizeof(event_data_wrap));
        if(send_event == NULL) {
            DPRINT(DPRINT_ERROR, "[%s] malloc() failed\n", __FUNCTION__);
            return (1);
        }

        memcpy(send_event, read_event, sizeof(event_data_wrap));

        send_event->eflags = (EV_WRITE | EV_PERSIST);
        send_event->callback = w_data_udp;
        send_event->params = send_event;

        send_event->eq = (echo_queue *) malloc(sizeof(echo_queue));
        memset(send_event->eq, 0, sizeof(echo_queue));
        pthread_mutex_init(&send_event->eq->lock, NULL);
        TAILQ_INIT(&send_event->eq->head);

        read_event->eq = send_event->eq;
        /* XXX this is evil... */
        send_event->eq->owner = (void *)send_event->eq;

        if(config_event(send_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
            free(send_event);

            *read_event->group_bp = NULL;
            destroy_event(read_event);
            free(read_event);
            return (1);
        }
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
        console_event->group_bp = NULL;

        if(config_event(console_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
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

        output_event->tv = (struct timeval *) calloc(1,
            sizeof(struct timeval));
        output_event->tv->tv_usec = 0;
        output_event->tv->tv_sec = rd->p.interval;

        output_event->params = output_event;
        output_event->group_bp = NULL;

        if(config_event(output_event) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n",
                __FUNCTION__);
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
    signal_event->group_bp = NULL;

    if(config_event(signal_event) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] unable to configure event\n", __FUNCTION__);
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


    if(bind(rd->s, (struct sockaddr *)&rd->saddr_s, rd->saddr_sz) < 0) {
        DPRINT(DPRINT_ERROR, "[%s] bind() failed \n", __FUNCTION__);
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
    rd->saddr_sz = sizeof(struct sockaddr_in);

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
    rd->saddr_sz = sizeof(struct sockaddr_in6);

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
    DPRINT(DPRINT_ERROR,"\t[-e <echo received data to sender>]\n");
    DPRINT(DPRINT_ERROR,"\t[-m <maximum allowable connections>]\n");
    DPRINT(DPRINT_ERROR,"Defaults:\n");
    DPRINT(DPRINT_ERROR,"\tSize of receive data: 256 bytes\n");
    DPRINT(DPRINT_ERROR,"\tDisplay status interval: 1 second\n");
}


int main(int argc, char *argv[])
{
    int ipver = 0;
    int len = 0;
    int opt;

    params p = {NULL, 0, 0, 0, BUFFER_SIZE, MAX_CONNECTIONS, INTERVAL, 0};
    run_data rd;

    while((opt = getopt(argc, argv, "4:6:p:t:S:i:m:de")) != -1) {
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

          case 'e':
              p.do_echo = 1;
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
    /* stats timer output events. if echo is enable, then max_cons should */
    /* be doubled: one for read and one for write on each connection      */
    if(p.do_echo)
        p.max_cons = (p.max_cons * 2) + 3;
    else
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
