/* necessary for getsubopt() on linux                                    */
/* man page for getsubopt(3) suggests using _XOPEN_SOURCE 500            */
/* but doing this breaks event.h code, googling throught mailing         */
/* led me to:                                                            */
/* http://www.linuxsa.org.au/pipermail/linuxsa/2005-February/077172.html */
/* which suggests using _GNU_SOURCE - apparently it works :)             */
#if defined (__linux__)
#define _GNU_SOURCE
#endif

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

#include <sys/time.h>
#include <time.h>
#include <event.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>

#define DEF_DELAY 1000000
#define DEF_INSTANCES 1


enum {
    SERVER = 0,
    CLIENT
};


char *const token[] = {
  [SERVER] = "server",
  [CLIENT] = "client",
  NULL
};


typedef struct _connection
{
  struct sockaddr_storage bindAddr;
  struct sockaddr_storage servAddr;
  socklen_t bindAddrSize;
  char *buffer;
  int buf_len;
  int cur_buf_len;
  int delay;
  int s;
  int ipver;
  pthread_t tid;
  int stop;
  int con_err;
  int established;
  int transport;
  int idx;
  pthread_mutex_t lock;
} connection;


typedef struct _event_timeout
{
  struct event e;
  struct timeval tv;
} event_timeout;


void sleep_random(void)
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


int get_ip_subopt(char **server, char **client, char *arg)
{
    int unknown = 0;
    char *value = NULL;

    while(*arg != '\0' && !unknown)
    {
        switch(getsubopt(&arg, token, &value))
        {
            case SERVER:
                *server = value;
                break;

            case CLIENT:
                *client = value;
                break;

            default:
                unknown = 1;
                break;
        }
    }

    return (unknown);
}


int is_val_set(int base, int val, pthread_mutex_t *l)
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


int set_val(int *base, int val, pthread_mutex_t *l)
{
    int ret;

    ret = pthread_mutex_trylock(l);
    if(ret == 0) {
        *base = val;
        pthread_mutex_unlock(l);    
        return (1);
    }
    
    return (0);
}


void dec_bufsize(int *size)
{
    /* assumption: size is always a multiple of 8 */
    int dec_size = (*size) - 8;
    *size = (dec_size > 0) ? dec_size : *size;
}


void inc_bufsize(int *size, int limit)
{
    /* assumption: size is always a multiple of 8 */
    int inc_size = (*size) + 8;
    *size = (inc_size <= limit) ? inc_size : *size;
}


void print_usage(char *cmd)
{
    DPRINT(DPRINT_ERROR,"Usage: %s [parameters]\n", cmd);
    DPRINT(DPRINT_ERROR,"Required parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[%s%s]\n", 
        "-4 server=<IPv4 address>,client=<IPv4 address>",
        " | -6 server=<IPv6 address>,client=<IPv6 address>");
    DPRINT(DPRINT_ERROR,"\t[-p <port number>]\n");
    DPRINT(DPRINT_ERROR,"\t[-t <transport protocol (tcp|udp)>]\n");
    DPRINT(DPRINT_ERROR,"Optional parameters:\n");
    DPRINT(DPRINT_ERROR,"\t[-S <size of data>]\n");
    DPRINT(DPRINT_ERROR,"\t[-i <number of thread instances>]\n");
    DPRINT(DPRINT_ERROR,"\t[-d <delay time>]\n");
    DPRINT(DPRINT_ERROR,"\t[-T <duration (in seconds)>]\n");
    DPRINT(DPRINT_ERROR,"Defaults:\n");
    DPRINT(DPRINT_ERROR,"\tDelay: 1000000 usec\n");
    DPRINT(DPRINT_ERROR,"\tNumber of instances: 1\n");
}


void *cb_run_client(void *arg)
{
    int est = 0;
    char *buffer = NULL;
    connection *cd_p = (connection *)arg;

    do {
        cd_p->s = socket(cd_p->ipver, cd_p->transport, 0);
        if(cd_p->s < 0) {
            DPRINT(DPRINT_ERROR, "[%s] socket() failed!\n", __FUNCTION__);
            break;
        }

        if(bind(cd_p->s, (struct sockaddr*)&cd_p->bindAddr, 
               cd_p->bindAddrSize) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] bind() failed!\n", __FUNCTION__);
            close(cd_p->s);
            break;
        }

        cd_p->buffer = (char *) malloc(cd_p->buf_len);

        if(cd_p->transport == SOCK_DGRAM) {
            set_val(&cd_p->established, 1, &cd_p->lock);
            est = 1;
            while(!is_val_set(cd_p->stop, 1, &cd_p->lock)) {
                sendto(cd_p->s, buffer, cd_p->buf_len, 0, 
                    (struct sockaddr *)&cd_p->servAddr, cd_p->bindAddrSize);
                usleep(cd_p->delay);
            }
        }
        else { /* assume it's SOCK_STREAM */
            if(connect(cd_p->s, (struct sockaddr *)&cd_p->servAddr, 
                   cd_p->bindAddrSize) == 0) {

                while(set_val(&cd_p->established, 1, &cd_p->lock) != 1) {
                    sleep_random();
                }
                est = 1;

                DPRINT(DPRINT_DEBUG, "[%s] connection #%d established!\n",
                      __FUNCTION__, cd_p->idx);

                while(!is_val_set(cd_p->stop, 1, &cd_p->lock)) {
                    send(cd_p->s, cd_p->buffer, cd_p->buf_len, 0);
                    usleep(cd_p->delay);
                }
            }
            else {
                while(set_val(&cd_p->con_err, 1, &cd_p->lock) != 1) {
                    sleep_random();
                }

                DPRINT(DPRINT_ERROR, "[%s] connection #%d connect() failed!\n",
                      __FUNCTION__, cd_p->idx);
            }
        }

        close(cd_p->s);
        free(buffer);

        DPRINT(DPRINT_DEBUG, "[%s] socket %d closed \n",
                      __FUNCTION__, cd_p->s);
    } while(0);

    if(est) {
        while(set_val(&cd_p->established, 0, &cd_p->lock) != 1) {
            sleep_random();
        }
    }

    return (NULL);
}


void cb_keyboard_int(int fd, short event, void *arg)
{
    int recv_sz = 0;
    char read_buff[80];
    struct event_base *b = (struct event_base *)arg;
    
    recv_sz = read(fd, (void *)read_buff, sizeof(read_buff));
    if(recv_sz > 0) {
        if(recv_sz == 1 && read_buff[0] == '\n') {
            event_base_loopbreak(b);
        }
    }
}


void cb_timeout(int fd, short event, void *arg)
{
    struct event_base *b = (struct event_base *)arg;
    event_base_loopbreak(b);
}


int main(int argc, char **argv)
{
    char *sip = NULL;
    char *cip = NULL;
    int i, opt;
    int ipver = 0;
    int port = 0;
    int buf_len = 0;
    int delay = DEF_DELAY;
    time_t tm_out = 0;
    int icount = DEF_INSTANCES;
    int len;
    int transport = 0;

    struct sockaddr_in sin;
    struct sockaddr_in6 sin6;
    struct event_base *ebase_halt = NULL;
    struct event e_ki;

    event_timeout e_timeout;
    connection c;
    connection *cons_p;

    while((opt = getopt(argc, argv, "4:6:p:t:S:d:T:i:h")) != -1)
    {
        switch(opt) {
            case '4':
                ipver = AF_INET;
                if(get_ip_subopt(&sip, &cip, optarg) != 0) {
                    print_usage(argv[0]);
                    return (1);
                }
                break;

            case '6':
                ipver = AF_INET6;
                if(get_ip_subopt(&sip, &cip, optarg) != 0) {
                    print_usage(argv[0]);
                    return (1);
                }
                break;

            case 'p':
                port = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 'S':
                buf_len = (int) strtol(optarg, (char **)NULL, 10);
                break; 

            case 'd':
                delay = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 't':
                len = strlen(argv[optind - 1]);

                if(!strncmp(argv[optind - 1], "tcp", len))
                    transport = SOCK_STREAM;
                else if(!strncmp(argv[optind - 1], "udp", len))
                    transport = SOCK_DGRAM;
                break;

            case 'T':
                tm_out = (time_t) strtol(optarg, (char **)NULL, 10);
                break;

            case 'i':
                icount = (int) strtol(optarg, (char **)NULL, 10);
                break;

            case 'h':
                print_usage(argv[0]);
                return (0);
                break;
     
            default:
                print_usage(argv[0]);
                return (1);
                break;
        }
    }

    if(cip == NULL || sip == NULL || port == 0 ||
         buf_len == 0 || transport == 0) {
        DPRINT(DPRINT_ERROR,"parameters are not valid\n");
        print_usage(argv[0]);
        return (1);
    } 

    memset(&c, 0, sizeof(connection));
    c.buffer = NULL;
    c.buf_len = buf_len;
    c.cur_buf_len = buf_len;
    c.delay = delay;
    c.ipver = ipver;
    c.transport = transport;

    if(c.ipver == AF_INET) {
        memset(&sin, 0, sizeof(struct sockaddr_in));

        sin.sin_family = AF_INET;
        if(inet_pton(AF_INET, sip, (void *)&sin.sin_addr) < 0) 
        {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, sip);
            return (1);
        }
        sin.sin_port = htons(port);
        memcpy(&c.servAddr, &sin, sizeof(struct sockaddr_storage));

        if(inet_pton(AF_INET, cip, (void *)&sin.sin_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, cip);
            return (1);
        }

        sin.sin_port = htons(0);
        memcpy(&c.bindAddr, &sin, sizeof(struct sockaddr_storage));

        c.bindAddrSize = sizeof(struct sockaddr_in);
    }
    else if (c.ipver == AF_INET6) {
        memset(&sin6, 0, sizeof(struct sockaddr_in6));

        sin6.sin6_family = AF_INET6;
        if(inet_pton(AF_INET6, sip, (void *)&sin6.sin6_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, sip);
            return (1);
        }
        sin6.sin6_port = htons(port);
        memcpy(&c.servAddr, &sin6, sizeof(struct sockaddr_storage));

        if(inet_pton(AF_INET6, cip, (void *)&sin6.sin6_addr) < 0) {
            DPRINT(DPRINT_ERROR, "[%s] failed to convert [%s] address\n", 
                __FUNCTION__, cip);
            return (1);
        }
        sin6.sin6_port = htons(0);
        memcpy(&c.bindAddr, &sin6, sizeof(struct sockaddr_storage));

        c.bindAddrSize = sizeof(struct sockaddr_in6);
    }

    daemon(0,0);

    ebase_halt = event_base_new();
    if(ebase_halt == NULL) {
        DPRINT(DPRINT_ERROR, "[%s] unable to initialize event base\n", 
            __FUNCTION__);
        return (1);
    }

    /* initialize keyboard interupt event handler */
    /*
    event_set(&e_ki, STDIN_FILENO, (EV_READ | EV_PERSIST), 
        cb_keyboard_int, ebase_halt);
    event_base_set(ebase_halt, &e_ki);
    event_add(&e_ki, NULL);
    */

    /* initialize timeout event handler */
    e_timeout.tv.tv_usec = 0;
    e_timeout.tv.tv_sec = tm_out;
    event_set(&e_timeout.e, -1, 0, cb_timeout, ebase_halt);
    event_base_set(ebase_halt, &e_timeout.e);
    event_add(&e_timeout.e, &e_timeout.tv);

    cons_p = (connection *) calloc(icount, sizeof(connection));

    for(i = 0; i < icount; ++i) {
        memcpy(&cons_p[i], &c, sizeof(connection));
        pthread_mutex_init(&cons_p[i].lock, NULL);
        cons_p[i].idx = i;
        DPRINT(DPRINT_DEBUG, "[%s] setting up connection #%d\n", __FUNCTION__, 
            cons_p[i].idx);
        if(pthread_create(&cons_p[i].tid, NULL, cb_run_client, 
               &cons_p[i]) != 0) {
            DPRINT(DPRINT_ERROR, "[%s] [%d] failed to run\n", __FUNCTION__, i);
        }

        while(!is_val_set(cons_p[i].established, 1, &cons_p[i].lock) &&
            is_val_set(cons_p[i].con_err, 0, &cons_p[i].lock)) {
                sleep_random();
        }

        /* prevent client from choking server */
        /* add heuristics to determine optimal value ?? */
        /*usleep(20000);*/
    }

    DPRINT(DPRINT_DEBUG, "[%s] running...\n", __FUNCTION__);

    /* this returns either on a timeout event or a keyboard event */
    event_base_dispatch(ebase_halt);

    DPRINT(DPRINT_DEBUG, "[%s] cleaning up...\n", __FUNCTION__);

    /* clean up */
    for(i = 0; i < icount; ++i) {
        if(!is_val_set(cons_p[i].established, 1, &cons_p[i].lock)) {
            pthread_mutex_destroy(&cons_p[i].lock);
        }
        else {
            /* tell thread to stop and do clean up */
            while(set_val(&cons_p[i].stop, 1, &cons_p[i].lock) != 1) {
                sleep_random();
            }

            DPRINT(DPRINT_DEBUG, "[%s] waiting for #%d\n",
                __FUNCTION__, cons_p[i].idx);
            while(!is_val_set(cons_p[i].established, 0, &cons_p[i].lock)) {
                sleep_random();
            }

            pthread_mutex_destroy(&cons_p[i].lock);
            DPRINT(DPRINT_DEBUG, "[%s] connection #%d closed\n",
                __FUNCTION__, cons_p[i].idx);
        }
    }

    free(cons_p);
    return (0);
}
