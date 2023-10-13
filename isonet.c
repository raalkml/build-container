/*
   cc -o isonet -D_GNU_SOURCE -Wall -O2 -ggdb isonet.c &&
   sudo sh -c 'chown root:root isonet && chmod 04751 isonet'
 */
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <wait.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <linux/loop.h>
#include <getopt.h>

#define NETDEV_MAX 100
static const char THENAME[] = "isonet";
static int verbose = 1;
static const char *PWD;
static const char *bridge;
static const char *inner_netdev;
static int use_dhcp;

static void error(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fputs(THENAME, stderr);
	fputs(": ", stderr);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

struct privileges
{
	unsigned has_uid:1,
		 has_gid:1;
	int ngroups;
	gid_t *groups;
	uid_t uid, euid;
	gid_t gid, egid;
	const char *user;
	const char *home;
};
static struct privileges privileges;

static int collect_sudo_privileges(const char *sudo_user)
{
	int ngroups, nalloc = 10;
	gid_t *groups = NULL;
	struct passwd *pw;

	errno = 0;
	pw = getpwnam(sudo_user);

	if (!pw) {
		error("SUDO_USER=\"%s\": %s\n", sudo_user,
		      errno ? strerror(errno): "user not found");
		return -1;
	}
	for (;;) {
		groups = realloc(groups, sizeof(*groups) * nalloc);
		if (!groups) {
			error("SUDO_USER=\"%s\": no memory for groups\n", sudo_user);
			return -1;
		}
		ngroups = nalloc;
		if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) >= 0)
			break;
		if (!ngroups) {
			free(groups);
			groups = NULL;
			break;
		}
		nalloc = ngroups > nalloc ? ngroups + 1: 2 * nalloc;
	}
	privileges.groups = groups;
	privileges.ngroups = ngroups;
	privileges.has_gid = 1;
	privileges.gid = pw->pw_gid;
	privileges.has_uid = 1;
	privileges.uid = pw->pw_uid;
	privileges.user = sudo_user;
	privileges.home = pw->pw_dir;
	return 0;
}

static int collect_privileges(void)
{
	uid_t ruid, suid;

	getresuid(&ruid, &privileges.euid, &suid);
	privileges.egid = getegid();
	if (ruid == privileges.euid) {
		char *sudo_user = getenv("SUDO_USER");
		if (sudo_user)
			return collect_sudo_privileges(sudo_user);
	}
	privileges.has_uid = 1;
	privileges.uid = ruid;
	return 0;
}

static int drop_privileges(void)
{
	if (privileges.has_gid && setregid(privileges.gid, privileges.gid) < 0) {
		error("setregid(%ld): %s\n", (long)privileges.gid, strerror(errno));
		return -1;
	}
	if (privileges.ngroups && setgroups(privileges.ngroups, privileges.groups) < 0) {
		error("setgroups: %s\n", strerror(errno));
		return -1;
	}
	privileges.ngroups = 0;
	free(privileges.groups);
	privileges.groups = NULL;
	if (privileges.has_uid && setreuid(privileges.uid, privileges.uid) < 0) {
		error("setreuid(%ld): %s\n", (long)privileges.uid, strerror(errno));
		return -1;
	}
	if (privileges.user) {
		setenv("USER", privileges.user, 1);
		setenv("USERNAME", privileges.user, 1);
		setenv("LOGNAME", privileges.user, 1);
	}
	if (privileges.home)
		setenv("HOME", privileges.home, 1);
	/* unsetenv("MAIL"); */
	return 0;
}

static int run_container(const char *cd_to, const char *prog, char **argv)
{
	if (drop_privileges())
		return 2;
	if (cd_to && chdir(cd_to) != 0)  {
		error("chdir(%s): %s\n", cd_to, strerror(errno));
		return 3;
	}
	if (verbose)
		fprintf(stderr, "%s: %s: pid %ld\n", THENAME, prog, (long)getpid());
	execvp(prog, argv);
	error("execvp(%s): %s\n", prog, strerror(errno));
	return 2;
}

static int setup_netns(void)
{
	struct ifreq ifr;
	int skfd = socket(AF_INET, SOCK_DGRAM, 0);

	if (skfd == -1) {
		error("netns: socket: %s\n", strerror(errno));
		return -1;
	}
	strcpy(ifr.ifr_name, "lo");
	if (ioctl(skfd, SIOCGIFFLAGS, &ifr) < 0) {
		error("netns: get interface flags: %s\n", strerror(errno));
		return -1;
	}
	ifr.ifr_flags |= IFF_UP;
	if (ioctl(skfd, SIOCSIFFLAGS, &ifr) < 0) {
		error("netns: set interface flags: %s\n", strerror(errno));
		return -1;
	}
	close(skfd);
	return 0;
}

static void usage(int code)
{
	fprintf(stderr, "%s [-hq] [-E NAME[=VALUE]] [-d <dir>] [-e <prog>] [-b <bridge>] [-- args...]\n"
		"\n"
		"Run the program <prog> in a new network namespace to isolate something.\n"
		"\n"
		"-h             show this text\n"
		"-q             disable printing of program and config file names\n"
		"-v             increase verbosity\n"
		"-b <bridge>    use <bridge> to connect the inner netdev to\n"
		"-e <prog>      run <prog> instead of ${SHELL:-/bin/sh}.\n"
		"-l             passed verbatim to the <prog>\n"
		"               (usually makes shell to act as if started as a login shell)\n"
		"-d <dir>       change current directory to <dir> before executing <prog>\n"
		"-w <dir>       same as -d <dir>, for docker-run compatibility\n"
		"-E NAME[=VALUE] set the environment variable NAME to the VALUE,\n"
		"               or unset the variable NAME if no VALUE given.\n"
		"\n",
		THENAME);
	exit(code);
}

struct netdev
{
	char name[32];
	pid_t pid;
	int fd;
};

#define ARGS_MAX 32
static int run(const char *arg0, ...)
{
	pid_t pid;
	int i = 0, status;
	const char *argv[ARGS_MAX];
	va_list args;

	va_start(args, arg0);
	for (; arg0 && i < ARGS_MAX; arg0 = va_arg(args, const char *)) {
		argv[i++] = arg0;
	}
	va_end(args);
	if (!i || i == ARGS_MAX)
		return -1;
	argv[i] = NULL;
	switch (pid = vfork()) {
	case -1:
		return -1;
	case 0:
		setuid(geteuid());
		execvp(argv[0], (char **)argv);
		error("exec(%s): %m\n", argv[0]);
		_exit(1);
	}
	do {
	    i = waitpid(pid, &status, 0);
	} while (i == -1 && EINTR == errno);
	if (i == -1)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -2;
}

#define RUN(...)					\
	({						\
		int __status = run(__VA_ARGS__, NULL);	\
		switch (__status) {			\
		case -1:				\
			error(#__VA_ARGS__ ": %m\n");	\
			exit(1);			\
		case -2:				\
			error(#__VA_ARGS__ ": failed\n");\
			exit(2);			\
		}					\
		__status;				\
	})

static void setup_outside_netdev(struct netdev *nd)
{
	char outdev[sizeof(nd->name) - 1];
	int i = 0;
nextdev:
	snprintf(outdev, sizeof(outdev), "isn%d", i);
	strcpy(nd->name, outdev);
	strcat(nd->name, "p");
	if (RUN("ip", "link", "add", outdev, "type", "veth", "peer", nd->name)) {
		++i;
		if (i > NETDEV_MAX)
			exit(1);
		goto nextdev;
	}

	if (RUN("ip", "link", "set", "dev", outdev, "master", bridge))
		goto cleanup;
	if (RUN("ip", "link", "set", "dev", outdev, "up"))
		goto cleanup;
	int fd[2];
	if (socketpair(AF_LOCAL, SOCK_DGRAM | SOCK_CLOEXEC, 0, fd) == -1) {
		error("socketpair: %m\n");
		exit(1);
	}
	switch (nd->pid = fork()) {
	case -1:
		error("fork(4): %m\n");
		exit(1);
	case 0:
		close(fd[1]);
		if (recv(fd[0], &fd[1], 1, 0) != 1)
			exit(1);
		snprintf(outdev, sizeof(outdev), "%ld", (long)getppid());
		execlp("ip", "ip", "link", "set", "dev", nd->name, "netns", outdev, NULL);
		error("exec(4): %m\n");
		exit(1);
	default:
		close(fd[0]);
		nd->fd = fd[1];
	}
	return;
cleanup:
	RUN("ip", "link", "del", "dev", outdev);
	exit(1);
}

static const char dhcp_script[] =
"#!/bin/sh\n"
//"set -x\n"
"case \"$1\" in\n"
"bound|renew)\n"
"    rc=$(mktemp -t resolv.XXXXXXX.conf) || exit 1\n"
"    ip link set dev \"$interface\" ${mtu:+mtu $mtu}\n"
"    ip -4 address add dev \"$interface\" \"$ip/$mask\" ${broadcast:+broadcast $broadcast}\n"
"    ip -4 route flush exact 0.0.0.0/0 dev \"$interface\"\n"
"    [ \".$subnet\" = .255.255.255.255 ] && onlink=onlink || onlink=\n"
"    ip -4 route add default via \"$router\" dev \"$interface\" $onlink\n"
"    > \"$rc\"\n"
"    [ -n \"$domain\" ] && echo \"domain $domain\" >> \"$rc\"\n"
"    for i in $dns; do\n"
"	echo \"nameserver $i\" >> \"$rc\"\n"
"    done\n"
"    umount /etc/resolv.conf 2>/dev/null\n"
"    chmod 0644 \"$rc\"\n"
"    mount --bind \"$rc\" /etc/resolv.conf\n"
"    rm -f \"$rc\"\n"
"    echo >&2 \"$interface: ipv4: $ip/mask dns: $dns\""
"    ;;\n"
"deconfig)\n"
"    umount /etc/resolv.conf\n"
"    ;;\n"
"leasefail|nak)\n"
"    echo >&2 \"$0: $1: $message\"\n"
"    ;;\n"
"*)\n"
"    echo >&2 \"$0: unknown command $1\"\n"
"esac\n";

static void setup_netdev(struct netdev *nd)
{
	int status;
	if (send(nd->fd, "1", 1, 0) != 1) {
		error("trigger netns setting (%m)\n");
		exit(1);
	}
	close(nd->fd);
	nd->fd = -1;
	waitpid(nd->pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		;
	else if (WIFSIGNALED(status))
		exit(128 + WTERMSIG(status));
	else
		exit(WEXITSTATUS(status));
	if (RUN("ip", "link", "set", "dev", nd->name, "name", inner_netdev))
		exit(1);
	if (RUN("ip", "link", "set", "dev", inner_netdev, "up"))
		exit(1);
	if (use_dhcp) {
		char dhcp_file[64] = "/tmp/dhcpXXXXXX";
		int fd = mkstemp(dhcp_file);
		if (fd < 0) {
			error("dhcp setup script: %m\n");
			exit(1);
		}
		write(fd, dhcp_script, sizeof(dhcp_script));
		fchmod(fd, 0755);
		close(fd);
		RUN("udhcpc", "-f", "-i", inner_netdev, "-s", dhcp_file, "-q");
		unlink(dhcp_file);
	}
}

int main(int argc, char *argv[])
{
	const char *prog = NULL;
	const char *cd_to = NULL;
	int opt, login = 0;

	PWD = get_current_dir_name();
	privileges.home = getenv("HOME");
	bridge = getenv("ISONET_BRIDGE");
	inner_netdev = getenv("ISONET_NETDEV");

	while ((opt = getopt(argc, argv, "he:lqd:w:vE:b:D")) != -1)
		switch (opt) {
			char *p;
		case 'h':
			usage(0);
			break;
		case 'b':
			bridge = optarg;
			break;
		case 'd':
		case 'w':
			cd_to = optarg;
			break;
		case 'e':
			prog = optarg;
			break;
		case 'E':
			p = strchr(optarg, '=');
			if (!p)
				unsetenv(optarg);
			else {
				*p = '\0';
				setenv(optarg, p + 1, 1);
			}
			break;
		case 'l':
			login = 1;
			break;
		case 'D':
			use_dhcp = 1;
			break;
		case 'q':
			verbose = 0;
			break;
		case 'v':
			++verbose;
			break;
		default:
			usage(1);
		}
	if (!prog) {
		if (verbose > 1)
			error("No program given, falling back to shell\n");
		prog = getenv("SHELL");
	}
	if (!prog)
		prog = "/bin/sh";
	if (!bridge)
		bridge =  "isonet0";
	if (!inner_netdev)
		inner_netdev = "eth0";
	if (login) {
		static char opt[] = "-l";
		argv[--optind] = opt;
	}
	/* collect privileges of the unmodified process environment */
	if (collect_privileges())
		exit(2);
	if (privileges.euid) {
		error("unprivileged execution\n");
		exit(1);
	}
	struct netdev nd;
	setup_outside_netdev(&nd);
	if (unshare(CLONE_NEWNS | CLONE_NEWNET) == 0) {
		if (setup_netns() != 0)
			exit(2);
		setup_netdev(&nd);
	} else {
		error("unshare(CLONE_NEWNET): %s\n", strerror(errno));
		exit(2);
	}
	argv[optind - 1] = (char *)prog;
	if (verbose) {
		int i;
		fprintf(stderr, "%s:%s%s starting '%s'", THENAME,
			cd_to ? cd_to : "", cd_to ? ":" : "", prog);
		for (i = optind; i < argc; ++i)
			fprintf(stderr, " '%s'", argv[i]);
		fputc('\n', stderr);
	}
	return run_container(cd_to, prog, argv + optind - 1);
}

