#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <signal.h>

#include <syslog.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define READ_SIZE 4096
#define TGT_FILE "/var/tmp/aesdsocketdata"

static int _run = 1;

void _handle_signal(int)
{
	// don't run next loop
	_run = 0;
}

int main(int argc, char **argv)
{
	int server_fd, client_fd;
	struct sockaddr_in server_addr, client_addr;
	int client_addr_len;
	int buffer_size = 0;
	char *buffer = NULL;
	char addr_buffer[16];
	int daemonize = 0;

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

	if(listen(server_fd, 1))
	{
		syslog(LOG_ERR, "failed to listen on socket: %s", strerror(errno));
		close(server_fd);
		return -1;
	}

	// init first buffer
	buffer_size = READ_SIZE;
	buffer = (char*)malloc(buffer_size);
	if(!buffer)
	{
		syslog(LOG_ERR, "failed to allocate memory: %s", strerror(errno));
		close(server_fd);
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

		// log
		syslog(LOG_DEBUG, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));

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
			nl = NULL;
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
			{
				/*
					new scope to keep variables local
					(gcc will reuse the space allocated to these)
				*/
				// write loop
				int to_write = nl-buffer+1;	// +1 to include the newline
				int total = to_write;
				int written = 0;
				while(written != total)
				{
					int write_status = write(file_fd, buffer+written, to_write);
					if(write_status < 0)
					{
						syslog(LOG_ERR, "failed to write to file: %s", strerror(errno));
						close(file_fd);
						goto _fini;	// skip send
					}
					written += write_status;
					to_write -= write_status;
				}
			}
			// send file to client
			{
				/* see above */
				int red;	// as in, past tense of "read", but without colliding names
				lseek(file_fd, 0, SEEK_SET);
				// reuse buffer
				while((red = read(file_fd, buffer, READ_SIZE)) > 0)
				{
					// send to client
					if(send(client_fd, buffer, red, 0) < 0)
					{
						syslog(LOG_ERR, "failed to send data to client: %s", strerror(errno));
						break;
					}
				}
				if(red < 0)
				{
					syslog(LOG_ERR, "failed to read data from file: %s", strerror(errno));
					// fallthrough
				}
				// else (red == 0) , EOF
			}
			// close file
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
		syslog(LOG_DEBUG, "Closed connection from %s", inet_ntoa(client_addr.sin_addr));
		// reset buffer
		{
			char *nbuffer = realloc(buffer, READ_SIZE);
			if(!nbuffer)
			{
				// why?
				syslog(LOG_ERR, "failed to allocate memory: %s", strerror(errno));
				break;
			}
			buffer = nbuffer;
			buffer_size = READ_SIZE;
		}
	}

	if(!_run)
	{
		// caught signal
		syslog(LOG_DEBUG, "Caught signal, exiting");
	}
	// delete file
	if(unlink(TGT_FILE))
	{
		syslog(LOG_ERR, "failed to remove file: %s", strerror(errno));
		// nothing we can do, fallthrough
	}

	// alright, close stuff
	free(buffer);
	close(server_fd);

	return 0;
}
