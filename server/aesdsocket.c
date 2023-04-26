#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <signal.h>
#include <time.h>

#include <syslog.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <pthread.h>

#include <sys/ioctl.h>

#include "aesd_ioctl.h"

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define READ_SIZE 512
#if USE_AESD_CHAR_DEVICE == 1
#define TGT_FILE "/dev/aesdchar"
#else
#define TGT_FILE "/var/tmp/aesdsocketdata"
#endif

struct thread_data {
	// shared mutex
	pthread_mutex_t *mutex;
	// socket connection
	int client_fd;
	// client address
	struct sockaddr_in client_addr;
};

struct ll_node {
	pthread_t tid;

	// the thread data, avoid multiple calls to malloc
	// will be passed to the thread
	struct thread_data td;

	struct ll_node *next;
};

static int _run = 1;

void _handle_signal(int sig)
{
	// don't run next loop
	_run = 0;
}

void * _do_thread(void *data)
{
	struct thread_data *td = (struct thread_data*)data;
	int client_fd = td->client_fd;

	// log
	syslog(LOG_DEBUG, "Accepted connection from %s", inet_ntoa(td->client_addr.sin_addr));

	int buffer_size = READ_SIZE;
	char *buffer = malloc(buffer_size);
	if(!buffer)
	{
		syslog(LOG_ERR, "failed to allocate memory: %s", strerror(errno));
		goto _fini;
	}

	// read loop
	int read_len = 0;
	int buffer_offset = 0;
	char *nl = NULL;
	while((read_len = recv(client_fd, buffer+buffer_offset, READ_SIZE, 0)) > 0)
	{
		// look for '\n'
		if((nl = memchr(buffer+buffer_offset, '\n', read_len)) != NULL)
			break;
		// else, allocate more space on buffer, and continue
		char *nbuffer = (char*)realloc(buffer, buffer_size + read_len);
		if(!nbuffer)
		{
			syslog(LOG_ERR, "failed to allocate memory: %s", strerror(errno));
			// set error for later
			nl = NULL;
			break;
		}
		// new buffer (may be the same)
		buffer = nbuffer;
		buffer_size += read_len;
		buffer_offset += read_len;
	}
	// if read_len < 0 --> failed to read from client
	if(read_len < 0)
	{
		syslog(LOG_ERR, "failed to read from client: %s", strerror(errno));
		// don't do rest of loop
		goto _fini;
	}
	// if read_len == 0 --> if it didn't read anything, it should've caught the _newline_, this shouldn't happen
	// if nl==NULL --> failed to allocate memory
	if(nl)
	{
		// we got a message (without errors)
		int file_fd = open(TGT_FILE, O_RDWR|O_APPEND|O_CREAT, 0644);
		if(file_fd < 0)
		{
			syslog(LOG_ERR, "failed to open/create file: %s", strerror(errno));
			// trying to avoid "goto"s, but ...
			goto _fini;
		}
		// append to file
#if USE_AESD_CHAR_DEVICE == 1
		if(!strncmp("AESDCHAR_IOCSEEKTO:", buffer, 19))
		{
			// just a seek through IOCTL
			char *str_x = buffer+19;
			char *str_y;
			struct aesd_seekto seekto;
			// assume no errors on strtol
			seekto.write_cmd = (uint32_t)strtol(str_x, &str_y, 10);
			// str_y will be at the comma
			seekto.write_cmd_offset = (uint32_t)strtol(str_y+1, NULL, 0);
			if(ioctl(file_fd, AESDCHAR_IOCSEEKTO, &seekto))
			{
				syslog(LOG_ERR, "failed to ioctl file: %s", strerror(errno));
				// with aesdchar, no mutexes are used
				goto _fini_file;
			}
		}
		else // normal read
#endif
		{
			/*
				new scope to keep variables local
				(gcc will reuse the space allocated to these)
			*/
			// write loop
			int to_write = nl-buffer+1;	// +1 to include the newline
			int total = to_write;
			int written = 0;
#if USE_AESD_CHAR_DEVICE != 1
			/*
				ACQUIRE MUTEX
			*/
			int r = pthread_mutex_lock(td->mutex);
			if(r)
			{
				syslog(LOG_ERR, "failed to acquire mutex: %s", strerror(r));
				goto _fini_file;
			}
#endif
			while(written != total)
			{
				int write_status = write(file_fd, buffer+written, to_write);
				if(write_status < 0)
				{
					syslog(LOG_ERR, "failed to write to file: %s", strerror(errno));
					// release lock
#if USE_AESD_CHAR_DEVICE != 1
					pthread_mutex_unlock(td->mutex);	// don't log errors
#endif
					goto _fini_file;	// skip send
				}
				written += write_status;
				to_write -= write_status;
			}
			//syncfs(file_fd);
#if USE_AESD_CHAR_DEVICE != 1
			if((r = pthread_mutex_unlock(td->mutex)) != 0)
			{
				syslog(LOG_ERR, "failed to release mutex: %s", strerror(r));
				goto _fini_file;
			}
			/*
				RELEASE MUTEX
			*/
#endif
		}
		// send file to client
		{
			/* see above */
			int red;	// as in, past tense of "read", but without colliding names
			int total_read = 0;
#if USE_AESD_CHAR_DEVICE != 1
			/*
				ACQUIRE MUTEX
			*/
			int r = pthread_mutex_lock(td->mutex);
			if(r)
			{
				syslog(LOG_ERR, "failed to acquire mutex: %s", strerror(r));
				goto _fini_file;
			}
#endif
			// reuse buffer
			// change to `pread`
			while((red = pread(file_fd, buffer, READ_SIZE, total_read)) > 0)
			{
				// send to client
				if(send(client_fd, buffer, red, 0) < 0)
				{
					syslog(LOG_ERR, "failed to send data to client: %s", strerror(errno));
					break;	// will release lock in a second
				}
				total_read += red;
			}
#if USE_AESD_CHAR_DEVICE != 1
			if((r = pthread_mutex_unlock(td->mutex)) != 0)
			{
				syslog(LOG_ERR, "failed to release mutex: %s", strerror(r));
			}
			/*
				RELEASE MUTEX
			*/
#endif
			if(red < 0)
			{
				syslog(LOG_ERR, "failed to read data from file: %s", strerror(errno));
				// fallthrough
			}
			// else (red == 0) , EOF
		}
		// close file
	_fini_file:
		if(close(file_fd))
		{
			syslog(LOG_ERR, "failed to close file: %s", strerror(errno));
			// fallthrough
		}
	}
	// else, carry one
_fini:
	// shutdown
	if(shutdown(client_fd, SHUT_RDWR))
	{
		// wierd?
		syslog(LOG_ERR, "failed to shutdown client connection: %s", strerror(errno));
	}
	// force close of fd
	close(client_fd);
	// log
	syslog(LOG_DEBUG, "Closed connection from %s", inet_ntoa(td->client_addr.sin_addr));
	// free buffer
	if(buffer) free(buffer);

	return NULL;
}

#if USE_AESD_CHAR_DEVICE != 1
void _timer_thread(union sigval sigval)
{
	pthread_mutex_t *mutex = (pthread_mutex_t*)sigval.sival_ptr;
	struct tm ret;
	time_t current_time;
	int r;
	int output_size = 0;
	char output[64] = "timestamp:";
	current_time = time(NULL);
	localtime_r(&current_time, &ret);
	r = strftime(output, 64, "timestamp:%a, %d %b %Y %T %z\n", &ret);
	if(!r)
	{
		// need more buffer
		syslog(LOG_ERR, "failed to strftime");
		return ;
	}
	output_size += r;
	int file_fd = open(TGT_FILE, O_WRONLY|O_APPEND|O_CREAT, 0644);
	if(file_fd < 0)
	{
		syslog(LOG_ERR, "failed to open/create file: %s", strerror(errno));
		return;
	}
	// lock
	if((r = pthread_mutex_lock(mutex)) != 0)
	{
		syslog(LOG_ERR, "failed to acquire mutex: %s", strerror(r));
		close(file_fd);
		return;
	}
	if(write(file_fd, output, output_size) != output_size)
	{
		syslog(LOG_ERR, "failed to write to file");
	}
	else
	{
		//syncfs(file_fd);
	}
	close(file_fd);
	if((r = pthread_mutex_unlock(mutex)) != 0)
	{
		syslog(LOG_ERR, "failed to release mutex: %s", strerror(r));
	}

	// all cool then
}
#endif

int main(int argc, char **argv)
{
	int server_fd, client_fd;
	struct sockaddr_in server_addr, client_addr;
	int client_addr_len;
	int daemonize = 0;
	struct ll_node *head = NULL, *tail = NULL;
	pthread_mutex_t mutex;
	int r;

	if(argc > 2)
	{
		fprintf(stderr, "usage: %s [-d]\n", *argv);
		return 1;
	}
	if(argc == 2 && !strcmp("-d", argv[1]))
		daemonize = 1;

	// setup signalling
	struct sigaction s_action = { 0 };
	s_action.sa_handler = _handle_signal;
	if(sigaction(SIGINT, &s_action, NULL) || sigaction(SIGTERM, &s_action, NULL))
	{
		syslog(LOG_ERR, "failed to setup signal handlers: %s", strerror(errno));
		return -1;
	}

	if((server_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	{
		syslog(LOG_ERR, "failed to create socket: %s", strerror(errno));
		return -1;
	}

	// reuse address
	{
		int val = 1;
		if(setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
		{
			// just warn, not critical
			syslog(LOG_WARNING, "failed to set socket to reuse address: %s", strerror(errno));
		}
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(9000);
	server_addr.sin_addr.s_addr = INADDR_ANY;
	if(bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)))
	{
		syslog(LOG_ERR, "failed to bind socket: %s", strerror(errno));
		close(server_fd);
		return -1;
	}

	// daemonize
	if(daemonize)
	{
		// TODO implement
		pid_t child_pid = fork();
		if(child_pid < 0)
		{
			syslog(LOG_ERR, "failed to fork: %s", strerror(errno));
			perror("failed to fork");
			return -1;
		}
		if(child_pid > 0)
		{
			// on parent
			exit(0);	// will release memory and fds
		}
		// else, on child
		if(setsid() < 0)
		{
			syslog(LOG_ERR, "failed to set session id: %s", strerror(errno));
			exit(-1);
		}
		if(chdir("/"))
		{
			syslog(LOG_ERR, "failed to change directory: %s", strerror(errno));
			exit(-1);
		}
		// only file descriptor is socket (and std*)
		// redirect std* > /dev/null
		// reopen as dev/null
		int dev_null_fd;
		if((dev_null_fd = open("/dev/null", O_RDWR)) < 0)
		{
			syslog(LOG_ERR, "failed to redirect stdin to /dev/null: %s", strerror(errno));
			exit(-1);
		}
		// redirect all
		dup2(dev_null_fd, STDIN_FILENO);
		dup2(dev_null_fd, STDOUT_FILENO);
		dup2(dev_null_fd, STDERR_FILENO);
		close(dev_null_fd);
	}

#if USE_AESD_CHAR_DEVICE != 1
	// setup sigaction for 10s timer
	timer_t se_timer;
	{
		// create timer
		struct sigevent se;
		memset(&se, 0, sizeof(se));
		se.sigev_notify = SIGEV_THREAD;
		se.sigev_value.sival_ptr = &mutex;
		se.sigev_notify_function = _timer_thread;
		if(timer_create(CLOCK_MONOTONIC, &se, &se_timer))
		{
			syslog(LOG_ERR, "failed to setup timer: %s", strerror(errno));
			close(server_fd);
			return -1;
		}
		// get current time
		struct timespec start_time;
		if(clock_gettime(CLOCK_MONOTONIC, &start_time))
		{
			syslog(LOG_ERR, "failed to get current time: %s", strerror(errno));
			timer_delete(se_timer);
			close(server_fd);
			return -1;
		}
		// setup timer
		struct itimerspec tspec;
		tspec.it_interval.tv_sec = 10;
		tspec.it_interval.tv_nsec = 0;
		tspec.it_value.tv_sec = start_time.tv_sec + tspec.it_interval.tv_sec;
		tspec.it_value.tv_nsec = start_time.tv_nsec + tspec.it_interval.tv_nsec;
		if(timer_settime(se_timer, TIMER_ABSTIME, &tspec, NULL))
		{
			syslog(LOG_ERR, "failed to set timer: %s", strerror(errno));
			timer_delete(se_timer);
			close(server_fd);
			return -1;
		}
	}
#endif

	// initialize mutex
	if((r = pthread_mutex_init(&mutex, NULL)) != 0)
	{
		syslog(LOG_ERR, "failed to initialize mutex: %s", strerror(r));
		close(server_fd);
#if USE_AESD_CHAR_DEVICE != 1
		timer_delete(se_timer);
#endif
		return -1;
	}

	if(listen(server_fd, 1))
	{
		syslog(LOG_ERR, "failed to listen on socket: %s", strerror(errno));
		close(server_fd);
#if USE_AESD_CHAR_DEVICE != 1
		timer_delete(se_timer);
#endif
		return -1;
	}

	// main loop
	while(_run)
	{
		client_addr_len = sizeof(client_addr);
		if((client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len)) < 0)
		{
			if(errno == EINTR)
				continue;
			syslog(LOG_ERR, "failed to accept client: %s", strerror(errno));
			//close(server_fd);
			break;
		}

		// create linked-list node
		struct ll_node *new = (struct ll_node*)malloc(sizeof(struct ll_node));
		if(!new)
		{
			syslog(LOG_ERR, "failed to allocate memory: %s", strerror(errno));
			close(client_fd);
			break;
		}
		memset(new, 0, sizeof(struct ll_node));
		new->td.client_addr = client_addr;
		new->td.client_fd = client_fd;
		new->td.mutex = &mutex;
		// start thread
		if((r = pthread_create(&new->tid, NULL, _do_thread, (void*)&new->td)) != 0)
		{
			syslog(LOG_ERR, "failed to start thread: %s", strerror(r));
			free(new);
			close(client_fd);
			break;
		}
		// append to list
		if(!head)
		{
			head = tail = new;
		}
		else
		{
			tail->next = new;
			tail = new;
		}
	}

	if(!_run)
	{
		// caught signal
		syslog(LOG_DEBUG, "Caught signal, exiting");
	}

	// iterate Linked-list, joining threads and freeing memory
	while(head)
	{
		struct ll_node *next = head->next;
		if((r = pthread_join(head->tid, NULL)) != 0)
		{
			syslog(LOG_ERR, "failed to join thread: %s", strerror(r));
			break;	// can we continue to iterate?
		}
		free(head);
		head = next;
	}

#if USE_AESD_CHAR_DEVICE != 1
	// delete file
	if(unlink(TGT_FILE))
	{
		syslog(LOG_ERR, "failed to remove file: %s", strerror(errno));
		// nothing we can do, fallthrough
	}

	timer_delete(se_timer);
#endif

	// alright, close stuff
	close(server_fd);

	return 0;
}
