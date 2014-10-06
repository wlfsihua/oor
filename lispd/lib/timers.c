/*
 * timers.c
 *
 * Timer maintenance routines. A simple, fixed granularity (1 second)
 * timer wheel implementation for scalable timers.
 *
 * Author: Chris White
 * Copyright 2012 Cisco Systems, Inc.
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>

#include "defs.h"
#include "lmlog.h"
#include "timers.h"
#include "util.h"
#include "lispd_external.h"


/* Seconds */
#define TICK_INTERVAL 1

/* Good for a little over an hour */
#define WHEEL_SIZE 4096

struct {
    int num_spokes;
    int current_spoke;
    lmtimer_links_t *spokes;
    timer_t tick_timer_id;
    int running_timers;
    int expirations;
} timer_wheel;

/* We don't have signalfd in bionic, fake it. */
static int signal_pipe[2];

static timer_t *timer_id;

/* timers file descriptor */
int timers_fd = 0;

static int destroy_timers_event_socket();
static int build_timers_event_socket(int *timers_fd);
static int process_timer_signal(sock_t *sl);
static void handle_timers(void);


/*
 * create_timer_wheel()
 *
 * Creates the timer wheel structure and starts
 * the rotation timer.
 */
static timer_t
create_timer_wheel(void)
{
    timer_t tid;
    struct sigevent sev;
    struct itimerspec timerspec;

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGRTMIN;
    sev.sigev_value.sival_ptr = &tid;
    if (timer_create(CLOCK_REALTIME, &sev, &tid) == -1) {
        LMLOG(DBG_1, "timer_create(): %s", strerror(errno));
        return (timer_t)0;
    }

    timerspec.it_value.tv_nsec = 0;
    timerspec.it_value.tv_sec = TICK_INTERVAL;
    timerspec.it_interval.tv_nsec = 0;
    timerspec.it_interval.tv_sec = TICK_INTERVAL;


    if (timer_settime(tid, 0, &timerspec, NULL) == -1) {
        LMLOG(DBG_2, "create_wheel_timer: timer start failed for %d %s",
               tid, strerror(errno));
        return (timer_t)0;
    }
    return(tid);
}


int
lmtimers_init()
{
    int i = 0;
    lmtimer_links_t *spoke;

    LMLOG(DBG_1, "Initializing lmtimers...");

    /* create timers event socket */
    if (build_timers_event_socket(&timers_fd) == 0) {
        LMLOG(LCRIT, " Error programming the timer signal. Exiting...");
        exit_cleanup();
    }


    timer_id = create_timer_wheel();
    if (timer_id == 0) {
        LMLOG(LINF, "Failed to set up timers.");
        return(BAD);
    }

    timer_wheel.num_spokes = WHEEL_SIZE;
    timer_wheel.spokes = xmalloc(sizeof(lmtimer_links_t) * WHEEL_SIZE);
    timer_wheel.current_spoke = 0;
    timer_wheel.running_timers = 0;
    timer_wheel.expirations = 0;

    spoke = &timer_wheel.spokes[0];
    for (i = 0; i < WHEEL_SIZE; i++) {
        spoke->next = spoke;
        spoke->prev = spoke;
        spoke++;
    }


    /* register timer fd with the socket master */
    sockmstr_register_read_listener(smaster, process_timer_signal, NULL,
            timers_fd);

    return(GOOD);
}

void
lmtimers_destroy()
{
    int i;
    lmtimer_links_t *spoke, *sit, *next;
    lmtimer_t *t;

    LMLOG(DBG_1, "Destroying lmtimers ... ");

    destroy_timers_event_socket();

    spoke = &timer_wheel.spokes[0];
    for (i = 0; i < WHEEL_SIZE; i++) {
        /* the first link is NOT a timer */
        sit = spoke->next;
        while (sit != spoke){
            next = sit->next;
            t = CONTAINER_OF(sit, lmtimer_t, links);
            lmtimer_stop(t);
            sit = next;
        }
        spoke++;
    }
    free(timer_wheel.spokes);
    timer_delete(timer_id);

}

/*
 * create_timer()
 *
 * Convenience function to allocate and zero a new timer.
 */
lmtimer_t *
lmtimer_create(char *name)
{
    lmtimer_t *new_timer = xzalloc(sizeof(lmtimer_t));
    strncpy(new_timer->name, name, TIMER_NAME_LEN - 1);
    new_timer->links.prev = NULL;
    new_timer->links.next = NULL;
    return(new_timer);
}

/* Insert a timer in the wheel at the appropriate location. */
static void
insert_timer(lmtimer_t *tptr)
{
    lmtimer_links_t *prev, *spoke;
    uint32_t pos;
    uint32_t ticks;
    uint32_t td;

    /* Number of ticks for this timer. */
    ticks = tptr->duration;

    /* tick position, referenced from the
     * current index. */
    td = (ticks % timer_wheel.num_spokes);

    /* Full rotations required before this timer expires */
    tptr->rotation_count = (ticks / timer_wheel.num_spokes);

    /* Find the right spoke, and link the timer into the list at this position */
    pos = ((timer_wheel.current_spoke + td) % timer_wheel.num_spokes);
    spoke = &timer_wheel.spokes[pos];

    /* append to end of spoke  */
    prev = spoke->prev;
    tptr->links.next = spoke;
    tptr->links.prev = prev;
    prev->next = (lmtimer_links_t *) tptr;
    spoke->prev = (lmtimer_links_t *) tptr;
    return;
}

/*
 * start_timer()
 *
 * Starts a new timer with given expiration time, callback function,
 * and arguments. Returns a pointer to the new timer, which must be kept
 * to stop the timer later if desired.
 */
void
lmtimer_start(lmtimer_t *tptr, int sexpiry, lmtimer_callback_t cb,
        void *owner, void *cb_arg)
{
    lmtimer_links_t *next, *prev;

    /* See if this timer is also running. */
    next = tptr->links.next;

    if (next != NULL) {
        prev = tptr->links.prev;
        next->prev = prev;
        prev->next = next;

        /* Update stats */
        timer_wheel.running_timers--;
    }

    tptr->owner = owner;

    /* Hook up the callback  */
    tptr->cb = cb;
    tptr->cb_argument = cb_arg;
    tptr->duration = sexpiry;
    insert_timer(tptr);

    timer_wheel.running_timers++;
    return;
}


/*
 * stop_timer()
 *
 * Mark one of the global timers as stopped and remove it.
 */
void
lmtimer_stop(lmtimer_t *tptr)
{
    lmtimer_links_t *next, *prev;

    if (tptr == NULL) {
        return;
    }

    next = tptr->links.next;
    prev = tptr->links.prev;
    if (next != NULL) {
        next->prev = prev;
    }
    if (prev != NULL) {
        prev->next = next;
    }
    tptr->links.next = NULL;
    tptr->links.prev = NULL;

    /* Update stats */
    if (next != NULL || prev != NULL) {
        timer_wheel.running_timers--;
    }
    free(tptr);
}


/*
 * handle_timers()
 *
 * Update the wheel index, and expire any timers there, calling
 * the appropriate function to deal with it.
 */
static void
handle_timers(void)
{
    struct timeval  nowtime;
    lmtimer_links_t    *current_spoke, *next, *prev;
    lmtimer_t          *tptr;
    lmtimer_callback_t  callback;

    gettimeofday(&nowtime, NULL);
    timer_wheel.current_spoke = (timer_wheel.current_spoke + 1) % timer_wheel.num_spokes;
    current_spoke = &timer_wheel.spokes[timer_wheel.current_spoke];

    tptr = (lmtimer_t *)current_spoke->next;
    while ((lmtimer_links_t *)tptr != current_spoke) {
        next = tptr->links.next;
        prev = tptr->links.prev;

        if (tptr->rotation_count > 0) {
            tptr->rotation_count--;
        } else {

            prev->next = next;
            next->prev = prev;
            tptr->links.next = NULL;
            tptr->links.prev = NULL;

            /* Update stats */
            timer_wheel.running_timers--;
            timer_wheel.expirations++;

            callback = tptr->cb;
            (*callback)(tptr, tptr->cb_argument);
        }
        /* We can not use directly "next" as it could be released  in the
         *  callback function  previously to be used */
        tptr = (lmtimer_t *)(prev->next);
    }
}

static int
process_timer_signal(sock_t *sl)
{
    int sig;
    int bytes;

    bytes = read(sl->fd, &sig, sizeof(sig));

    if (bytes != sizeof(sig)) {
        LMLOG(LWRN, "process_event_signal(): nothing to read");
        return(-1);
    }

    if (sig == SIGRTMIN) {
        handle_timers();
    }
    return(0);
}



/*
 * event_sig_handler
 *
 * Forward signal to the fd for handling in the event loop
 */
static void event_sig_handler(int sig)
{
    if (write(signal_pipe[1], &sig, sizeof(sig)) != sizeof(sig)) {
        LMLOG(LWRN, "write signal %d: %s", sig, strerror(errno));
    }
}


/*
 * build_timer_event_socket
 *
 * Set up the event handler socket. This is
 * used to serialize events like timer expirations that
 * we would rather deal with synchronously. This avoids
 * having to deal with all sorts of locking and multithreading
 * nonsense.
 */
static int
build_timers_event_socket(int *timers_fd)
{
    int flags;
    struct sigaction sa;

    if (pipe(signal_pipe) == -1) {
        LMLOG(LERR, "build_timers_event_socket: signal pipe setup failed %s",
                strerror(errno));
        return (BAD);
    }
    *timers_fd = signal_pipe[0];

    if ((flags = fcntl(*timers_fd, F_GETFL, 0)) == -1) {
        LMLOG(LERR, "build_timers_event_socket: fcntl() F_GETFL failed %s",
                strerror(errno));
        return (BAD);
    }
    if (fcntl(*timers_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LMLOG(LERR, "build_timers_event_socket: fcntl() set O_NONBLOCK failed "
                "%s", strerror(errno));
        return (BAD);
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = event_sig_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        LMLOG(LERR, "build_timers_event_socket: sigaction() failed %s",
                strerror(errno));
        exit_cleanup();
    }
    return(GOOD);
}

static int
destroy_timers_event_socket()
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = NULL;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
        LMLOG(LERR, "destroy_timers_event_socket: sigaction() failed %s",
                strerror(errno));
    }

    close(signal_pipe[0]);
    close(signal_pipe[1]);
    return(GOOD);
}
