#include "threading.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

// Optional: use these functions to add debug or error prints to your application
#define DEBUG_LOG(msg,...)
//#define DEBUG_LOG(msg,...) printf("threading: " msg "\n" , ##__VA_ARGS__)
#define ERROR_LOG(msg,...) printf("threading ERROR: " msg "\n" , ##__VA_ARGS__)

void* threadfunc(void* thread_param)
{

    // TODO: wait, obtain mutex, wait, release mutex as described by thread_data structure
    // hint: use a cast like the one below to obtain thread arguments from your parameter
    //struct thread_data* thread_func_args = (struct thread_data *) thread_param;
    struct thread_data *td = (struct thread_data*)thread_param;
    int err;
    // default value
    td->thread_complete_success = false;

    /*
        time is specified in milliseconds,
        `usleep` uses microseconds
    */
    if(usleep(td->wait_to_obtain_ms * 1000))
    {
        perror("failed to sleep");
        return (void*)td;
    }

    if((err = pthread_mutex_lock(td->mutex)) != 0)
    {
        fprintf(stderr, "failed to lock mutex: %s\n", strerror(err));
        return (void*)td;
    }

    // assume everything is okay
    td->thread_complete_success = true;

    if(usleep(td->wait_to_relase_ms * 1000))
    {
        perror("failed to sleep");
        // just set to false
        td->thread_complete_success = false;
        // we always need to release lock here
    }

    if((err = pthread_mutex_unlock(td->mutex)) != 0)
    {
        fprintf(stderr, "failed to release mutex: %s\n", strerror(err));
        // will return
    }

    return thread_param;
}


bool start_thread_obtaining_mutex(pthread_t *thread, pthread_mutex_t *mutex,int wait_to_obtain_ms, int wait_to_release_ms)
{
    /**
     * TODO: allocate memory for thread_data, setup mutex and wait arguments, pass thread_data to created thread
     * using threadfunc() as entry point.
     *
     * return true if successful.
     *
     * See implementation details in threading.h file comment block
     */
    struct thread_data *td = (struct thread_data*)malloc(sizeof(struct thread_data));
    if(!td)
    {
        perror("failed to allocate memory");
        return false;
    }
    td->mutex = mutex;
    td->wait_to_obtain_ms = wait_to_obtain_ms;
    td->wait_to_relase_ms = wait_to_release_ms;
    td->thread_complete_success = false;

    // start thread
    int err;
    if((err = pthread_create(thread, NULL, threadfunc, (void*)td)) != 0)
    {
        fprintf(stderr, "failed to create thread: %s\n", strerror(err));
        free(td);
        return false;
    }

    // else
    return true;
}
