#include <stdio.h>
#include <syslog.h>

int main(int argc, char**argv)
{
	openlog(NULL, 0, LOG_USER);
	if(argc != 3)
	{
		syslog(LOG_ERR, "arguments not specified\n");
		return 1;
	}
	char *fname = argv[1];
	char *string = argv[2];

	syslog(LOG_DEBUG, "Writing %s to %s\n", string, fname);
	FILE *f = fopen(fname, "w");
	if(!f)
	{
		syslog(LOG_ERR, "failed to open %s\n", fname);
		return 1;
	}

	if(fputs(string, f) == EOF)
	{
		syslog(LOG_ERR, "failed to write to %s\n", fname);
		return 1;
	}

	if(fclose(f) == EOF)
	{
		syslog(LOG_ERR, "failed to close file\n");
		return 1;
	}

	// all good then
	return 0;
}
