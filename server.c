#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include "./comm.h"
#include "./db.h"

/*
 * Use the variables in this struct to synchronize your main thread with client
 * threads. Note that all client threads must have terminated before you clean
 * up the database.
 */
typedef struct server_control {
    pthread_mutex_t server_mutex;
    pthread_cond_t server_cond;
    int num_client_threads;
} server_control_t;

/*
 * Controls when the clients in the client thread list should be stopped and
 * let go.
 */
typedef struct client_control {
    pthread_mutex_t go_mutex;
    pthread_cond_t go;
    int stopped;
} client_control_t;

/*
 * The encapsulation of a client thread, i.e., the thread that handles
 * commands from clients.
 */
typedef struct client {
    pthread_t thread;
    FILE *cxstr;  // File stream for input and output

    // For client list
    struct client *prev;
    struct client *next;
} client_t;

/*
 * The encapsulation of a thread that handles signals sent to the server.
 * When SIGINT is sent to the server all client threads should be destroyed.
 */
typedef struct sig_handler {
    sigset_t set;
    pthread_t thread;
} sig_handler_t;

// initialize various structs and global information
client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;
int accepting_clients = 1;
server_control_t s_vars = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                           0};
client_control_t c_vars = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER,
                           0};

// forward declarations
void *run_client(void *arg);
void *monitor_signal(void *arg);
void thread_cleanup(void *arg);

// called by client threads to wait until progress is permitted
void client_control_wait() {
    // block the calling thread until the main
    // thread calls client_control_release()
    pthread_mutex_lock(&c_vars.go_mutex);
    pthread_cleanup_push((void (*)(void *))pthread_mutex_unlock,
                         (void *)&c_vars.go_mutex);
    while (c_vars.stopped == 1) {
        pthread_cond_wait(&c_vars.go, &c_vars.go_mutex);
    }
    pthread_cleanup_pop(1);
}

// called by main thread to stop client threads
void client_control_stop() {
    // ensure that the next time client threads call client_control_wait()
    // at the top of the event loop in run_client, they will block.
    pthread_mutex_lock(&c_vars.go_mutex);
    c_vars.stopped = 1;
    pthread_mutex_unlock(&c_vars.go_mutex);
}

// called by main thread to resume client threads
void client_control_release() {
    // allow clients blocked in client_control_wait() to proceed
    pthread_mutex_lock(&c_vars.go_mutex);
    c_vars.stopped = 0;
    pthread_cond_broadcast(&c_vars.go);
    pthread_mutex_unlock(&c_vars.go_mutex);
}

/* called by listener (in comm.c) to create a new client thread
 *  cxstr is a file pointer to the I/O stream for the client
 */
void client_constructor(FILE *cxstr) {
    // allocate memory for a new client and set its connection stream to cxstr
    client_t *new_client = (client_t *)malloc(sizeof(client_t));

    new_client->cxstr = cxstr;

    // create the new client thread running the run_client routine.
    int err;
    if ((err = pthread_create(&new_client->thread, 0,
                              (void *(*)(void *))run_client,
                              (void *)new_client))) {
        handle_error_en(err, "pthread_create");
    }

    // detach the new client thread
    if ((err = pthread_detach(new_client->thread))) {
        handle_error_en(err, "pthread_detach");
    }
}

void client_destructor(client_t *client) {
    // free and close all resources associated with a client
    comm_shutdown(client->cxstr);
    free(client);
}

/*
 * code executed by a client thread. adds itself to the client list
 *  and
 * arg should be a pointer to a client_t
 */
void *run_client(void *arg) {
    // ensure that the server is still accepting clients.
    if (!accepting_clients) {
        return (void *)1;
    }

    // ------------ add client to the list, setup ------------
    client_t *client = (client_t *)arg;

    pthread_mutex_lock(&thread_list_mutex);

    // add client to front of list
    if (thread_list_head == NULL) {
        thread_list_head = client;
    } else {
        client->next = thread_list_head;
        client->prev = NULL;
        thread_list_head->prev = client;
        thread_list_head = client;
    }

    pthread_mutex_unlock(&thread_list_mutex);

    // increment number of clients
    pthread_mutex_lock(&s_vars.server_mutex);
    s_vars.num_client_threads++;
    pthread_mutex_unlock(&s_vars.server_mutex);

    // push cleanup handler to stack
    pthread_cleanup_push(thread_cleanup, client);

    // ------------------ client pseudo-REPL ------------------
    // create and initilize response and command buffers
    char response[BUFLEN];
    char command[BUFLEN];
    memset(response, 0, BUFLEN);
    memset(command, 0, BUFLEN);

    // loop comm_serve until we reach EOF/termination
    while (comm_serve(client->cxstr, response, command) != -1) {
        // check if server is blocking client commands
        client_control_wait();
        // safe to proceed to execution
        interpret_command(command, response, BUFLEN);
    }

    // when the client is done, exit the thread cleanly
    pthread_cleanup_pop(1);

    return NULL;
}

/*
 * cancels all threads in the client list, each of which will
 *  have pushed thread_cleanup as their cleanup routine
 */
void delete_all() {
    pthread_mutex_lock(&s_vars.server_mutex);
    client_t *curr = thread_list_head;
    client_t *temp;

    // cancel every thread in the client thread list
    while (curr != NULL) {
        temp = curr->next;
        pthread_cancel(curr->thread);
        curr = temp;
    }
    pthread_mutex_unlock(&s_vars.server_mutex);
}

/* cleanup routine for client threads, called on cancels and exits.
 * arg should be a pointer to the client_t in question
 */
void thread_cleanup(void *arg) {
    // ------------------ remove client from list -------------------
    client_t *client = (client_t *)arg;

    pthread_mutex_lock(&thread_list_mutex);
    // if client is only member of the list
    if (client->prev == NULL && client->next == NULL) {
        thread_list_head = NULL;
    }
    // client is at the end of the list
    else if (client->next == NULL) {
        client->prev->next = NULL;
    }
    // client is at the head of the list
    else if (client == thread_list_head) {
        thread_list_head = client->next;
    }
    // client is somewhere in the middle of the list
    else {
        client->prev->next = client->next;
        client->next->prev = client->prev;
    }

    pthread_mutex_unlock(&thread_list_mutex);

    // destroy client
    client_destructor(client);

    // ------------- check if last client thread -------------
    pthread_mutex_lock(&s_vars.server_mutex);

    // we just deleted a client, decrement number of threads
    s_vars.num_client_threads--;

    if (s_vars.num_client_threads == 0) {
        // if no more clients, we can signal to cleanup the database
        pthread_cond_broadcast(&s_vars.server_cond);
    }

    pthread_mutex_unlock(&s_vars.server_mutex);
}

/* code executed by the signal handler thread.
 *  all of the server's client threads should terminate on SIGINT,
 *  but the server will not.
 * arg should be the sigset to wait on (only SIGINT)
 */
void *monitor_signal(void *arg) {
    int err;

    // SIGPIPE and SIGINT should already be masked
    int waitsig;
    sigset_t *waitset = (sigset_t *)arg;

    while (1) {
        // wait for SIGINT
        if ((err = sigwait(waitset, &waitsig))) {
            handle_error_en(err, "sigwait: SIGINT");
        }

        // once we get SIGINT, delete all clients, and wait for next signal
        if (waitsig == SIGINT) {
            if ((err = fprintf(stdout, "\nSIGINT received.\n")) < 0) {
                handle_error_en(err, "fprintf");
            }
            fflush(stdout);
            delete_all();
        }
    }

    return (void *)1;
}

/*
 * creates a sighandler for SIGINT in a separate thread
 *  this will be the only thread to respond to SIGINT
 */
sig_handler_t *sig_handler_constructor() {
    // initialize a sighandler
    sig_handler_t *sighandler = (sig_handler_t *)malloc(sizeof(sig_handler_t));
    sigemptyset(&sighandler->set);
    sigaddset(&sighandler->set, SIGINT);

    // block SIGINT
    int err;
    if ((err = pthread_sigmask(SIG_BLOCK, &sighandler->set, NULL))) {
        free(sighandler);
        handle_error_en(err, "pthread_sigmask");
    }

    // create a thread to handle SIGINT
    if ((err = pthread_create(&sighandler->thread, 0,
                              (void *(*)(void *))monitor_signal,
                              &sighandler->set))) {
        free(sighandler);
        handle_error_en(err, "pthread_create");
    }

    return sighandler;
}

/*
 * cancels and joins a sighandler's thread with the main thread
 *  also freeing any resources associated with it
 * sighandler should be a pointer to a sig_handler_t
 */
void sig_handler_destructor(sig_handler_t *sighandler) {
    int err;

    // cancel and join with main thread
    if ((err = pthread_cancel(sighandler->thread))) {
        handle_error_en(err, "pthread_cancel on sighandler");
    }

    if ((err = pthread_join(sighandler->thread, NULL))) {
        handle_error_en(err, "pthread_join");
    }

    // free sighandler resources
    free(sighandler);
}

// the argument to the server should be the port number
int main(int argc, char *argv[]) {
    // get port number from argv
    if (argc != 2) {
        fprintf(stderr, "Usage: ./server <port>\n");
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port < 1024) {
        fprintf(stderr, "Error: port must be at least 1024.\n");
        exit(EXIT_FAILURE);
    }

    // --------------------- signal handling ----------------------
    sigset_t set;
    int err;

    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);

    if ((err = pthread_sigmask(SIG_BLOCK, &set, NULL))) {
        handle_error_en(err, "pthread_sigmask");
    }

    // set up the signal handler for handling SIGINT
    sig_handler_t *sig_int_handler = sig_handler_constructor();

    // start a listener thread for clients
    pthread_t listener_id;
    listener_id = start_listener(port, &client_constructor);

    // --------------------- input REPL -----------------------
    int num_bytes_read;
    char command[BUFLEN];
    memset(command, 0, BUFLEN);

    while (1) {
        num_bytes_read = read(STDIN_FILENO, command, BUFLEN);

        // check for EOF
        if (num_bytes_read == 0) {
            accepting_clients = 0;
            break;
        }
        // check for reading error
        if (num_bytes_read == -1) {
            if ((err = fprintf(stderr, "ERROR - read error.")) < 0) {
                handle_error_en(err, "fprintf");
            }
            exit(EXIT_FAILURE);
        }

        // stop command
        if (command[0] == 's') {
            client_control_stop();
        }

        // go command
        else if (command[0] == 'g') {
            client_control_release();
        }

        // print command
        else if (command[0] == 'p') {
            char *filename = strtok(&command[1], " \t\n\r");
            if ((err = db_print(filename))) {
                handle_error_en(err, "db_print fopen");
            }
        }

        memset(command, 0, BUFLEN);
    }

    // --------------------- Post-EOF Cleanup -------------
    // destroy signal handler
    sig_handler_destructor(sig_int_handler);

    // cancel and join with listener thread
    if ((err = pthread_cancel(listener_id))) {
        handle_error_en(err, "pthread_cancel on listener");
    }
    if ((err = pthread_join(listener_id, NULL))) {
        handle_error_en(err, "pthread_join");
    }

    // delete all clients after listener is closed
    //  so we are guaranteed no new additions
    delete_all();

    // ensure we're down to 0 client threads before cleaning up database
    pthread_mutex_lock(&s_vars.server_mutex);
    while (s_vars.num_client_threads) {
        pthread_cond_wait(&s_vars.server_cond, &s_vars.server_mutex);
    }
    pthread_mutex_unlock(&s_vars.server_mutex);

    // assured we have 0 client threads, safe to cleanup database
    db_cleanup();

    // exit main thread cleanly
    pthread_exit((void *)0);
    return 0;
}
