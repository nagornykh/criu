#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <linux/sched.h>
#include <sched.h>
#include <signal.h>

#include "zdtmtst.h"

const char *test_doc = "Test split TUN devices\n";
const char *test_author = "Dmitry Nagornykh <dmitry.nagornykh@openvz.org>";

#define TUN_DEVICE "/dev/net/tun"

static int any_fail=0;

static int __open_tun(void)
{
	int fd;

	fd = open(TUN_DEVICE, O_RDWR);
	if (fd < 0)
		pr_perror("Can't open tun file %s", TUN_DEVICE);

	return fd;
}

static int __attach_tun(int fd, const char *name, unsigned flags)
{
	struct ifreq ifr = {
		.ifr_flags = flags,
	};

	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name) - 1);

	if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
		if (!(flags & IFF_TUN_EXCL))
			pr_perror("Can't attach iff %s", name);
		return -1;
	}

	return fd;
}

static int open_tun(const char *name, unsigned flags)
{
	int fd = __open_tun();
	if (fd < 0)
		return -1;

	return __attach_tun(fd, name, flags);
}

static void check_tun(int fd, const char *name, unsigned flags)
{
	struct ifreq ifr = {};

	if (ioctl(fd, TUNGETIFF, &ifr) > 0) {
		any_fail = 1;
		fail("Attached tun %s file lost device", name);
	}

	if (strcmp(ifr.ifr_name, name)) {
		any_fail = 1;
		fail("Attached tun %s wrong device", name);
	}

	if ((ifr.ifr_flags & flags) != flags) {
		any_fail = 1;
		fail("Attached tun %s wrong device type", name);
	}
}

int get_fdns_by_pid(pid_t pid) 
{
	int fd;
	char buff[256];

	sprintf(buff, "/proc/%d/ns/net", pid);
	fd=open(buff, O_RDONLY);
	if (fd < 0) {
		pr_perror("Cannot open namespace fd %s", buff);
	}
	return fd;
}

unsigned long get_ino(int fd, int code)
{
	struct stat st;
	int nsfd;

	if (code>=0) 
		nsfd = ioctl(fd, code);
	else 
		nsfd = fd;

	if (nsfd < 0) {
		pr_perror("Unable to get a net namespace fd");
		return 0;
	}

	if (fstat(nsfd, &st)) {
		pr_perror("Unable to stat a net namespace");
		st.st_ino = 0;
	}
	
	close(nsfd);
	return st.st_ino;
}

int check_tun_ns(int ftun, pid_t pid1)
{	
	pid_t pid2 = getpid();
	unsigned long ino_sock_ns = get_ino(ftun, SIOCGSKNS);
	unsigned long ino_netdev_ns = get_ino(ftun, TUNGETDEVNETNS);	
	unsigned long ino_ns1 = get_ino(get_fdns_by_pid(pid1), -1);	
	unsigned long ino_ns2 = get_ino(get_fdns_by_pid(pid2), -1);

	bool fl1 = ino_sock_ns==ino_ns1 || ino_sock_ns==ino_ns2;
	bool fl2 = ino_netdev_ns==ino_ns1 || ino_netdev_ns==ino_ns2;
	
	if (ino_sock_ns==ino_netdev_ns || ino_ns2==ino_ns1 || !fl1 || !fl2) {
		pr_perror("Wrong tun ns, ns_sock=%ld ns_netdev=%ld ns_1=%ld ns_2=%ld", 
			ino_sock_ns, ino_netdev_ns, ino_ns1, ino_ns2);		
		return 1;
	}

	return 0;
}

int prepare_netns(void)
{
	if (unshare(CLONE_NEWNET)) {
		pr_perror("unshare");
		return 1;
	}
	return system("ip link set up dev lo") < 0 ? 1 : 0;
}

int move_netdev_to_ns(const char* tun_name, pid_t pid)  
{
	char buff[64];
	sprintf(buff,"ip link set %s netns %d", tun_name, pid);
	return system(buff) < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{	
	int kid_pid;
	const char *tun1 = "tunA1";
	int fd;

	test_init(argc, argv);

	if ( prepare_netns() )
		return 1;

	fd = open_tun(tun1, IFF_TUN);
	if (fd<0) {
		pr_perror("No tun1");
		return 1;
	}


	kid_pid = fork();
	if (kid_pid == -1) {
        perror("No fork");
        return 1;
    }

	if (kid_pid == 0) {
		if ( prepare_netns() )
			return 1;

		while (1)
			sleep(1);

		return 0;
	}
	else {
		if (move_netdev_to_ns(tun1, kid_pid)) {
			pr_perror("Can't move to another NS");
			return 1;
		}
	
		if (check_tun_ns(fd, kid_pid))
			return 1;
	}


	test_daemon();
	test_waitsig();

	check_tun(fd, tun1, IFF_TUN);

	if ( check_tun_ns(fd, kid_pid) ) 
		any_fail = 1;	

	if (!any_fail)
		pass();

	return 0;
}
