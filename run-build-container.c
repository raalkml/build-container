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
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/if.h>
#include <linux/sockios.h>
#include <getopt.h>

#ifndef BUILD_CONTAINER_PATH
#define BUILD_CONTAINER_PATH "BUILD_CONTAINER_PATH"
#endif
#ifndef CONTAINER_PATH
#define CONTAINER_PATH "~/.config/build-container:/etc/build-container"
#endif

static const char build_container[] = "build-container";
static int check_config;
static int verbose = 1;
static int chrooted;
static int pidns;
static int netns;

static void error(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fputs(build_container, stderr);
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
	uid_t uid;
	gid_t gid;
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
	uid_t ruid, euid, suid;

	getresuid(&ruid, &euid, &suid);
	if (ruid == euid) {
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

enum arg {
	WORK,
	FROM,
	TO,
};

struct stk
{
	struct stk *next;
	enum arg arg;
	char val[1];
};

static void push(struct stk **head, enum arg arg, const char *val)
{
	struct stk *e = malloc(sizeof(struct stk) + strlen(val));

	e->next = *head;
	e->arg = arg;
	strcpy(e->val, val);
	*head = e;
}

static struct stk *pop(struct stk **head)
{
	struct stk *e = *head;

	if (!e)
		return NULL;
	*head = e->next;
	return e;
}

#define swap(a, b) do { typeof(a) _t = b; b = a; a = _t; } while (0)

static char spaces[] = "\x20\t\r";

static int at_line_terminator(const char *s)
{
	return '\n' == *s || !*s;
}

static int at_id_terminator(const char *s)
{
	return strchr(spaces, *s) || at_line_terminator(s);
}

static int is_absolute(const char *path)
{
	return path && '/' == *path;
}

static int expect_id(const char *sym, char **s)
{
	char *p = *s;
	size_t n = strlen(sym);

	if (strncasecmp(sym, p, n))
		return 0;
	p += n;
	if (!at_id_terminator(p))
		return 0;
	*s = p;
	return 1;
}

static char *cleanup(char *s)
{
	size_t n;

	s += strspn(s, spaces);
	n = strlen(s);
	if (n > 1 && (memcmp(s + n - 2, "\r\n", 2) == 0 ||
		      memcmp(s + n - 2, "\n\r", 2) == 0))
		s[n - 2] = '\0';
	else if (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[n - 1] = '\0';
	return s;
}

static char *abspath_buf;
static const char *abspath(const char *dir, const char *name)
{
	static size_t size;
	size_t newsize;

	if (is_absolute(name))
		return name;
	if (name[0] == '~' && (name[1] == '/' || !name[1])) {
		dir = getenv("HOME");
		++name;
		if (*name)
			++name;
	}
	newsize = strlen(dir) + strlen(name) + 2;
	if (newsize > size)
		abspath_buf = realloc(abspath_buf, size = newsize);
	strcpy(abspath_buf, dir);
	if (abspath_buf[strlen(abspath_buf) - 1] != '/')
		strcat(abspath_buf, "/");
	strcat(abspath_buf, name);
	return abspath_buf;
}

static int do_mount_options(unsigned long *opts, char *arg)
{
	arg = cleanup(arg);
	while (!at_line_terminator(arg)) {
		arg += strspn(arg, spaces);
		if (expect_id("rec", &arg))
			*opts |= MS_REC;
		else if (expect_id("noexec", &arg))
			*opts |= MS_NOEXEC;
		else if (expect_id("nosuid", &arg))
			*opts |= MS_NOSUID;
		else if (expect_id("nodev", &arg))
			*opts |= MS_NODEV;
		else if (expect_id("ro", &arg))
			*opts |= MS_RDONLY;
		else {
			error("syntax error: mount option unsupported: %s\n", arg);
			return -1;
		}
	}
	return 0;
}

static int do_mount(char *src, char *tgt, const char *fstype,
		    unsigned long flags, const void *data,
		    char *args)
{
	unsigned long opts = 0;

	// FIXME validate if the src and tgt are accessible by the target user
	if (do_mount_options(&opts, args) != 0)
		return -1;
	if (check_config) {
		printf("# mount %s %s %s 0x%lx%s %s\n",
		       src, tgt, fstype, flags | opts,
		       flags & MS_BIND ? " bind" :
		       flags & MS_MOVE ? " move" : "",
		       (const char *)data);
		return 0;
	}
	if (mount(src, tgt, fstype, flags | (opts & MS_REC ? MS_REC : 0), data) != 0) {
		error("%smount(%s, %s): %s\n",
		      flags & MS_BIND ? "bind " :
		      flags & MS_MOVE ? "move " : "",
		      src, tgt, strerror(errno));
		return -1;
	}
	if (opts & ~(unsigned long)MS_REC) {
		if (mount(src, tgt, fstype, MS_REMOUNT | flags | opts, data) != 0) {
			error("%smount(%s, %s, 0x%lx): %s\n",
			      flags & MS_BIND ? "bind " :
			      flags & MS_MOVE ? "move " : "",
			      src, tgt, opts, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int do_chroot(const char *root)
{
	chrooted = 1;
	if (check_config) {
		printf("# chroot '%s'\n", root);
		return 0;
	}
	if (chroot(root) == 0)
		return 0;
	error("chroot(%s): %s\n", root, strerror(errno));
	return -1;
}

static FILE *open_config_file(const char *file, char **dir)
{
	FILE *fp = fopen(file, "r");

	if (fp) {
		const char *e = file + strlen(file);

		while (e > file)
			if ('/' == *--e)
				break;
		if (e == file)
			*dir = strdup("./");
		else
			*dir = strndup(file, e - file + 1);
		if (verbose || check_config)
			fprintf(check_config ? stdout : stderr, "# config file '%s'\n", file);
	} else if (check_config) {
		int err = errno;
		printf("# config file '%s': %s\n", file, strerror(err));
		errno = err;
	}
	return fp;
}

static FILE *open_config(const char *config, char **config_dir)
{
	static char dot[] = ".";
	FILE *fp;
	char *dirs;
	char *p;

	if (is_absolute(config))
		return open_config_file(config, config_dir);
	dirs = getenv(BUILD_CONTAINER_PATH);
	if (dirs)
		/* Necessary: protecting environment
		   for subsequent users of the variable. */
		dirs = strdup(dirs);
	else
		dirs = strdup(CONTAINER_PATH);
	if (!*dirs) {
		free(dirs);
		error("No defined path for configuration file %s\n", config);
		errno = EINVAL;
		return NULL;
	}
	char *end = dirs + strlen(dirs) + 1;
	for (p = dirs; p < end;) {
		char *file, *next;
		size_t n = strcspn(p, ":");
		next = p + n + 1;
		if (!n) {
			p = dot;
			n = 1;
		}
		if (p[0] == '~' && (p[1] == '/' || p[1] == ':' || !p[1])) {
			const char *home = getenv("HOME");
			if (!home)
				home = dot + 1;
			file = malloc(strlen(home) + n + strlen(config) + 2);
			strcpy(file, home);
			memcpy(file + strlen(file), p + 1, n - 1);
			file[strlen(file) + n - 1] = '\0';
		} else {
			file = malloc(n + strlen(config) + 2);
			memcpy(file, p, n);
			file[n] = '\0';
		}
		strcat(file, "/");
		strcat(file, config);
		fp = open_config_file(file, config_dir);
		if (fp)
			break;
		p = next;
	}
	free(dirs);
	return fp;
}

static int do_config(const char *config)
{
	char line[BUFSIZ];
	struct stk *head = NULL, *a, *b;
	int ret = 0;
	char *config_dir;
	FILE *fp = open_config(config, &config_dir);

	if (!fp) {
		error("config file: %s: %s\n", config, strerror(errno));
		return -1;
	}
	while (fgets(line, BUFSIZ, fp)) {
		char *arg = line + strspn(line, spaces);

		if ('#' == *arg)
			continue;
		/*
		 * XXX the paths are delimited by EOL token and cannot begin with
		 * XXX a whitespace character: all leading space is removed.
		 */
		if (expect_id("from", &arg))
			push(&head, FROM, abspath(config_dir, cleanup(arg)));
		else if (expect_id("to", &arg))
			push(&head, TO, abspath(config_dir, cleanup(arg)));
		else if (expect_id("work", &arg))
			push(&head, WORK, abspath(config_dir, cleanup(arg)));
		else if (expect_id("bind", &arg)) {
			b = pop(&head);
			a = pop(&head);
			if (a && a->arg != FROM)
				swap(a, b);
			if (a && b && a->arg == FROM && b->arg == TO)
				ret = do_mount(a->val, b->val, NULL, MS_BIND, NULL, arg);
			else {
				error("'bind' expects 'from' and 'to' paths\n");
				ret = -1;
			}
			free(a);
			free(b);
		}
		else if (expect_id("move", &arg)) {
			b = pop(&head);
			a = pop(&head);
			if (a && a->arg != FROM)
				swap(a, b);
			if (a && b && a->arg == FROM && b->arg == TO)
				ret = do_mount(a->val, b->val, NULL, MS_MOVE, NULL, arg);
			else {
				error("'move' expects 'from' and 'to' paths\n");
				ret = -1;
			}
			free(a);
			free(b);
		}
		else if (expect_id("union", &arg)) {
			struct stk *e;
			/* collect all 'from' paths and exactly one 'to' */
			size_t lowersize = 0;
			a = b = NULL;
			while (head) {
				switch (head->arg) {
				case FROM:
					e = pop(&head);
					lowersize += strlen(e->val) + 1;
					e->next = a;
					a = e;
					break;
				case TO:
					if (b)
						ret = -2;
					else
						b = pop(&head);
				default:
					break;
				}
			}
			if (ret == -2 || !a || !b) {
				ret = -1;
				error("'union' expects exactly one 'to' path "
				      "and at least one from\n");
			} else {
				char *data = malloc(sizeof("lowerdir") + lowersize);
				strcpy(data, "lowerdir=");
				for (e = a; e; e = e->next) {
					strcat(data, e->val);
					if (e->next)
						strcat(data, ":");
				}
				ret = do_mount("union", b->val, "overlay", 0, data, arg);
				free(data);
			}
			free(b);
			while (a) {
				e = a->next;
				free(a);
				a = e;
			}
		}
		else if (expect_id("overlay", &arg)) {
			struct stk *e, *w;
			/* collect one 'work', two 'from' paths, and exactly one 'to' */
			w = a = b = NULL;
			while (head && ret == 0 && !(a && a->next && b && w)) {
				switch (head->arg) {
				case WORK:
					if (w)
						ret = -2;
					else
						w = pop(&head);
					break;
				case FROM:
					if (a && a->next)
						ret = -2;
					else {
						e = pop(&head);
						e->next = a;
						a = e;
					}
					break;
				case TO:
					if (b)
						ret = -2;
					else
						b = pop(&head);
					break;
				}
			}
			if (ret == -2 || !(a && a->next) || !b || !w) {
				ret = -1;
				error("'overlay' expects exactly one 'work', two 'from', "
				      "and one 'to' path lines\n");
			} else {
				char *data = malloc(sizeof("lowerdir=,upperdir=,workdir=") + 
						    strlen(a->val) + strlen(a->next->val) +
						    strlen(b->val) + strlen(w->val));
				sprintf(data, "upperdir=%s,lowerdir=%s,workdir=%s",
					a->val, a->next->val, w->val);
				ret = do_mount("overlay", b->val, "overlay", 0, data, arg);
				free(data);
			}
			free(w);
			free(b);
			while (a) {
				e = a->next;
				free(a);
				a = e;
			}
		}
		else if (expect_id("chroot", &arg)) {
			ret = do_chroot(abspath(config_dir, cleanup(arg)));
		}
		if (ret)
			break;
	}
	fclose(fp);
	free(config_dir);
	while (head) {
		a = head->next;
		free(head);
		head = a;
	}
	return ret;
}

static int run_container(const char *cd_to, const char *prog, char **argv)
{
	if (drop_privileges())
		return 2;
	if (cd_to && chdir(cd_to) != 0)  {
		error("chdir(%s): %s\n", cd_to, strerror(errno));
		return 3;
	}
	execvp(prog, argv);
	error("execvp(%s): %s\n", prog, strerror(errno));
	return 2;
}

static int run_pidns_container(const char *cd_to, const char *prog, char **argv)
{
	if (unshare(CLONE_NEWPID) != 0) {
		error("unshare(CLONE_NEWPID): %s\n", strerror(errno));
		return 2;
	}
	switch (fork()) {
		int status;
	case -1:
		error("fork(%s): %s\n", prog, strerror(errno));
		break;
	case 0:
		if (verbose)
			fprintf(stderr, "%s: %s: pid %ld\n", build_container, prog, (long)getpid());
		if (pidns > 1 && mount("proc", "/proc", "proc", 0, NULL) != 0) {
			error("mount(proc): %s\n", strerror(errno));
			exit(2);
		}
		if (drop_privileges())
			exit(2);
		if (cd_to && chdir(cd_to) != 0)  {
			error("chdir(%s): %s\n", cd_to, strerror(errno));
			exit(3);
		}
		execvp(prog, argv);
		error("execvp(%s): %s\n", prog, strerror(errno));
		_exit(2);
	default:
		/*
		 * Currently, dropping privileges here is not strictly
		 * speaking necessary. Drop them anyway just in case.
		 */
		(void)drop_privileges();
		while (wait(&status) == -1)
			if (EINTR != errno) {
				error("wait(%s): %s\n", prog, strerror(errno));
				return 2;
			}
		if (WIFEXITED(status)) {
			if (verbose > 1)
				fprintf(stderr, "%s finished (%d)\n", prog, WEXITSTATUS(status));
			return WEXITSTATUS(status);
		}
		else if (WIFSIGNALED(status)) {
			error("%s: %s\n", prog, strsignal(WTERMSIG(status)));
			return 128 + WSTOPSIG(status);
		}
		error("failed(%s)\n", prog);
		return 127;
	}
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
	fprintf(stderr, "%s [-hqcLP] [-n <container>] [-d <dir>] [-e <prog>] [-- args...]\n"
		"-h             show this text\n"
		"-q             disable printing of program and config file names\n"
		"-v             increase verbosity\n"
		"-n <container> read configuration from $"BUILD_CONTAINER_PATH" if set, or from\n"
		"               "CONTAINER_PATH"/container\n"
		"               (instead of just unsharing namespaces).\n"
		"-e <prog>      run <prog> instead of ${SHELL:-/bin/sh}.\n"
		"-c             check configuration only, don't run anything.\n"
		"-L             lock file system inside the container from all\n"
		"               changes in the outside (parent) namespace, i.e. unmounts.\n"
		"-l             passed verbatim to the <prog>\n"
		"               (usually makes shell to act as if started as a login shell)\n"
		"-d <dir>       change current directory to <dir> before executing <prog>\n"
		"-P             unshare the pid namespace to avoid run-away build processes.\n"
		"               Given twice, will also mount a new /proc in the container\n"
		"-N             unshare the network namespace to allow, for instance, multiple\n"
		"               services on the same local TCP or UNIX ports or remove network\n"
		"               access from the build container (loopback interface will be set up)\n"
		"\n",
		build_container);
	exit(code);
}

int main(int argc, char *argv[])
{
	const char *config = NULL;
	const char *prog = NULL;
	const char *cd_to = NULL;
	int opt, lock_fs = 0, login = 0;

	while ((opt = getopt(argc, argv, "hn:e:cLlqd:PNv")) != -1)
		switch (opt) {
		case 'h':
			usage(0);
			break;
		case 'n':
			config = optarg;
			break;
		case 'd':
			cd_to = optarg;
			break;
		case 'e':
			prog = optarg;
			break;
		case 'c':
			check_config = 1;
			break;
		case 'L':
			lock_fs = 1;
			break;
		case 'l':
			login = 1;
			break;
		case 'q':
			verbose = 0;
			break;
		case 'v':
			++verbose;
			break;
		case 'P':
			++pidns;
			break;
		case 'N':
			++netns;
			if (netns > 1)
				usage(1);
			break;
		default:
			usage(1);
		}
	if (!prog)
		prog = getenv("SHELL");
	if (!prog)
		prog = "/bin/sh";
	if (login) {
		static char opt[] = "-l";
		argv[--optind] = opt;
	}
	/* collect privileges of the unmodified process environment */
	if (collect_privileges())
		exit(2);
	if (check_config) {
		if (drop_privileges())
			exit(2);
		if (config && do_config(config) != 0)
			exit(3);
		if (chrooted && !cd_to)
			cd_to = get_current_dir_name();
		if (cd_to)
			printf("# cd '%s'\n", cd_to);
		printf("# starting '%s'", prog);
		for (; optind < argc; ++optind)
			printf(" '%s'", argv[optind]);
		fputc('\n', stdout);
		exit(0);
	}
	if (unshare(CLONE_NEWNS | (netns ? CLONE_NEWNET : 0)) == 0) {
		if (mount("none", "/", NULL,
			  MS_REC | (lock_fs ? MS_PRIVATE : MS_SLAVE), NULL) != 0) {
			error("setting mount propagation: %s\n", strerror(errno));
			exit(2);
		}
		if (netns && setup_netns() != 0)
			exit(2);
	} else {
		error("unshare(CLONE_NEWNS): %s\n", strerror(errno));
		exit(2);
	}
	/* FIXME that's a bit careless: reading and parsing with full privileges */
	if (config && do_config(config) != 0)
		exit(3);
	if (chrooted && !cd_to)
		cd_to = get_current_dir_name();
	argv[optind - 1] = (char *)prog;
	if (verbose) {
		int i;
		fprintf(stderr, "%s:%s%s starting '%s'", build_container,
			cd_to ? cd_to : "", cd_to ? ":" : "", prog);
		for (i = optind; i < argc; ++i)
			fprintf(stderr, " '%s'", argv[i]);
		fputc('\n', stderr);
	}
	if (pidns)
		return run_pidns_container(cd_to, prog, argv + optind - 1);
	return run_container(cd_to, prog, argv + optind - 1);
}

