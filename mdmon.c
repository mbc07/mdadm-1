
/*
 * md array manager.
 * When md arrays have user-space managed metadata, this is the program
 * that does the managing.
 *
 * Given one argument: the name of the array (e.g. /dev/md0) that is
 * the container.
 * We fork off a helper that runs high priority and mlocked.  It responds to
 * device failures and other events that might stop writeout, or that are
 * trivial to deal with.
 * The main thread then watches for new arrays being created in the container
 * and starts monitoring them too ... along with a few other tasks.
 *
 * The main thread communicates with the priority thread by writing over
 * a pipe.
 * Separate programs can communicate with the main thread via Unix-domain
 * socket.
 * The two threads share address space and open file table.
 *
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include	<unistd.h>
#include	<stdlib.h>
#include	<sys/stat.h>
#include	<sys/socket.h>
#include	<sys/un.h>
#include	<sys/mman.h>
#include	<stdio.h>
#include	<errno.h>
#include	<string.h>
#include	<fcntl.h>

#include	<sched.h>

#include	"mdadm.h"
#include	"mdmon.h"

struct active_array *array_list;
struct active_array *discard_this;
struct active_array *pending_discard;

int run_child(void *v)
{
	struct supertype *c = v;
	do_monitor(c);
	return 0;
}

int clone_monitor(struct supertype *container)
{
	int pfd[2];
	static char stack[4096];
	int rv;

	pipe(container->pipe);

	rv = clone(run_child, stack+4096-64,
		   CLONE_FS|CLONE_FILES|CLONE_VM|CLONE_SIGHAND|CLONE_THREAD,
		   container);

	if (rv < 0)
		return rv;
	return pfd[1];
}

static struct superswitch *find_metadata_methods(char *vers)
{
	if (strcmp(vers, "ddf") == 0)
		return &super_ddf;
	return NULL;
}


static int make_pidfile(char *devname)
{
	char path[100];
	char pid[10];
	int fd;
	sprintf(path, "/var/run/mdadm/%s.pid", devname);

	fd = open(path, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (fd < 0)
		return -1;
	sprintf(pid, "%d\n", getpid());
	write(fd, pid, strlen(pid));
	close(fd);
	return 0;
}

static int make_control_sock(char *devname)
{
	char path[100];
	int sfd;
	long fl;
	struct sockaddr_un addr;

	sprintf(path, "/var/run/mdadm/%s.sock", devname);
	unlink(path);
	sfd = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (sfd < 0)
		return -1;

	addr.sun_family = PF_LOCAL;
	strcpy(addr.sun_path, path);
	if (bind(sfd, &addr, sizeof(addr)) < 0) {
		close(sfd);
		return -1;
	}
	listen(sfd, 10);
	fl = fcntl(sfd, F_GETFL, 0);
	fl |= O_NONBLOCK;
	fcntl(sfd, F_SETFL, fl);
	return sfd;
}

int main(int argc, char *argv[])
{
	int mdfd;
	int pipefd;
	struct mdinfo *mdi, *di;
	struct supertype *container;
	if (argc != 2) {
		fprintf(stderr, "Usage: md-manage /device/name/for/container\n");
		exit(2);
	}
	mdfd = open(argv[1], O_RDWR);
	if (mdfd < 0) {
		fprintf(stderr, "md-manage: %s: %s\n", argv[1],
			strerror(errno));
		exit(1);
	}
	if (md_get_version(mdfd) < 0) {
		fprintf(stderr, "md-manage: %s: Not an md device\n",
			argv[1]);
		exit(1);
	}

	/* hopefully it is a container - we'll check later */

	container = malloc(sizeof(*container));
	container->devfd = mdfd;
	container->devnum = fd2devnum(mdfd);
	container->devname = devnum2devname(container->devnum);

	/* If this fails, we hope it already exists */
	mkdir("/var/run/mdadm", 0600);
	/* pid file lives in /var/run/mdadm/mdXX.pid */
	if (make_pidfile(container->devname) < 0) {
		fprintf(stderr, "md-manage: %s already managed\n",
			container->devname);
		exit(3);
	}

	container->sock = make_control_sock(container->devname);
	if (container->sock < 0) {
		fprintf(stderr, "mdmon: Cannot create socket in /var/run/mdadm\n");
		exit(3);
	}
	container->arrays = NULL;

	mdi = sysfs_read(mdfd, container->devnum,
			 GET_VERSION|GET_LEVEL|GET_DEVS);

	if (!mdi) {
		fprintf(stderr, "mdmon: failed to load sysfs info for %s\n",
			container->devname);
		exit(3);
	}
	if (mdi->array.level != UnSet) {
		fprintf(stderr, "mdmon: %s is not a container - cannot monitor\n",
			argv[1]);
		exit(3);
	}
	if (mdi->array.major_version != -1 ||
	    mdi->array.minor_version != -2) {
		fprintf(stderr, "mdmon: %s does not use external metadata - cannot monitor\n",
			argv[1]);
		exit(3);
	}

	container->ss = find_metadata_methods(mdi->text_version);
	if (container->ss == NULL) {
		fprintf(stderr, "mdmon: %s uses unknown metadata: %s\n",
			argv[1], mdi->text_version);
		exit(3);
	}

	container->devs = NULL;
	for (di = mdi->devs; di; di = di->next) {
		struct mdinfo *cd = malloc(sizeof(*cd));
		cd = di;
		cd->next = container->devs;
		container->devs = cd;
	}
	sysfs_free(mdi);


	if (container->ss->load_super(container, mdfd, argv[1])) {
		fprintf(stderr, "mdmon: Cannot load metadata for %s\n",
			argv[1]);
		exit(3);
	}


	mlockall(MCL_FUTURE);

	pipefd = clone_monitor(container);
	if (pipefd < 0) {
		fprintf(stderr, "md-manage: failed to start monitor process: %s\n",
			strerror(errno));
		exit(2);
	}

	do_manager(container);

	exit(0);
}