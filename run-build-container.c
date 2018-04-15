#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/mount.h>
#include <pwd.h>
#include <grp.h>
#include <getopt.h>

#ifndef BUILD_CONTAINER_PATH
#define BUILD_CONTAINER_PATH "BUILD_CONTAINER_PATH"
#endif
#ifndef CONTAINER_PATH
#define CONTAINER_PATH "~/.config/build-container:/etc/build-container"
#endif

static int check_config;
static int verbose = 1;

static int drop_sudo_privileges(const char *sudo_user)
{
	int ngroups, nalloc = 10;
	gid_t *groups;
	struct passwd *pw;

	errno = 0;
	pw = getpwnam(sudo_user);

	if (!pw) {
		fprintf(stderr, "SUDO_USER=\"%s\": %s\n", sudo_user,
			errno ? strerror(errno): "user not found");
		return -1;
	}
	for (;;) {
		groups = malloc(sizeof(gid_t) * nalloc);
		ngroups = nalloc;
		if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
			nalloc = ngroups > nalloc ? ngroups + 1: 2 * nalloc;
		else
			break;
	}
	if (setregid(pw->pw_gid, pw->pw_gid) < 0) {
		perror("SUDO_USER (gid)");
		return -1;
	}
	if (setgroups(ngroups, groups) < 0) {
		perror("SUDO_USER (groups)");
		return -1;
	}
	free(groups);
	if (setreuid(pw->pw_uid, pw->pw_uid) < 0) {
		perror("SUDO_USER (uid)");
		return -1;
	}
	setenv("USER", sudo_user, 1);
	setenv("USERNAME", sudo_user, 1);
	setenv("LOGNAME", sudo_user, 1);
	setenv("HOME", pw->pw_dir, 1);
	/* unsetenv("MAIL"); */
}

static int drop_privileges(void)
{
	uid_t ruid, euid, suid;
	getresuid(&ruid, &euid, &suid);
	char *sudo_user = getenv("SUDO_USER");
	if (sudo_user)
		return drop_sudo_privileges(sudo_user);
	if (setresuid(ruid, ruid, ruid) < 0) {
		perror("setresuid");
		return -1;
	}
	return 0;
}

enum arg {
	FROM,
	TO,
};

struct stk
{
	struct stk *next;
	enum arg arg;
	char val[0];
};

static void push(struct stk **head, enum arg arg, const char *val)
{
	struct stk *e = malloc(sizeof(struct stk) + strlen(val) + 1);

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
			fprintf(stderr, "syntax error: mount option unsupported: %s\n", arg);
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

	if (do_mount_options(&opts, args) != 0)
		return -1;
	if (check_config) {
		printf("# mount %s %s %s 0x%lx%s %s\n",
		       src, tgt, fstype, flags | opts,
		       flags & MS_BIND ? " bind" :
		       flags & MS_MOVE ? " move" : "",
		       data);
		return 0;
	}
	if (mount(src, tgt, fstype, flags | (opts & MS_REC ? MS_REC : 0), data) != 0) {
		fprintf(stderr, "mount(%s, %s): %s\n", src, tgt,
			strerror(errno));
		return -1;
	}
	if (opts & ~(unsigned long)MS_REC) {
		if (mount(src, tgt, fstype, MS_REMOUNT | flags | opts, data) != 0) {
			fprintf(stderr, "mount(%s, %s, 0x%lx): %s\n", src, tgt, opts,
				strerror(errno));
			return -1;
		}
	}
	return 0;
}

static FILE *open_config_file(const char *config)
{
	static char dot[] = ".";
	FILE *fp;
	char *dirs = getenv(BUILD_CONTAINER_PATH);
	char *p;

	if (dirs)
		/* Necessary: protecting environment
		   for subsequent users of the variable. */
		dirs = strdup(dirs);
	else
		dirs = strdup(CONTAINER_PATH);
	if (!*dirs) {
		free(dirs);
		fprintf(stderr, "No defined path for configuration file %s\n", config);
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
		fp = fopen(file, "r");
		if (fp) {
			if (verbose || check_config)
				fprintf(check_config ? stdout : verbose  ? stderr : NULL,
					"# config file '%s'\n", file);
			break;
		}
		if (check_config) {
			int err = errno;
			printf("# config file '%s': %s\n", file, strerror(err));
			errno = err;
		}
		p = next;
	}
	free(dirs);
	return fp;
}

static int do_config(const char *config)
{
	FILE *fp = open_config_file(config);
	char line[BUFSIZ];
	struct stk *head = NULL, *a, *b;
	int ret = 0;

	if (!fp) {
		fprintf(stderr, "%s: %s\n", config, strerror(errno));
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
			push(&head, FROM, cleanup(arg));
		else if (expect_id("to", &arg))
			push(&head, TO, cleanup(arg));
		else if (expect_id("bind", &arg)) {
			b = pop(&head);
			a = pop(&head);
			if (a && a->arg != FROM)
				swap(a, b);
			if (a && b && a->arg == FROM && b->arg == TO)
				ret = do_mount(a->val, b->val, NULL, MS_BIND, NULL, arg);
			else {
				fprintf(stderr, "'bind' expects 'from' and 'to' paths\n");
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
				fprintf(stderr, "'move' expects 'from' and 'to' paths\n");
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
					break;
				}
			}
			if (ret == -2 || !a || !b) {
				ret = -1;
				fprintf(stderr, "'union' expects exactly one 'to' path "
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
		if (ret)
			break;
	}
	fclose(fp);
	return ret;
}

static void usage(const char *argv0, int code)
{
	fprintf(stderr, "%s [-hqcL] [-n <container>] [-e <prog>] [-- args...]\n"
		"-h             show this text\n"
		"-q             disable printing of program and config file names\n"
		"-n <container> read configuration from {"CONTAINER_PATH"}/container\n"
		"               (instead of just unsharing mount namespace).\n"
		"-e <prog>      run <prog> instead of ${SHELL:-/bin/sh}.\n"
		"-c             check configuration only, don't run anything.\n"
		"-L             lock file system inside the container from all\n"
		"               changes in the outside (parent) namespace, i.e. unmounts.\n"
		"-l             passed verbatim to the <prog>\n"
		"               (usually makes shell to act as if started as a login shell)\n"
		"\n",
		argv0);
	exit(code);
}

int main(int argc, char *argv[])
{
	const char *config = NULL;
	const char *prog = NULL;
	int opt, lock_fs = 0, login = 0;

	while ((opt = getopt(argc, argv, "hn:e:cLlq")) != -1)
		switch (opt) {
		case 'h':
			usage(argv[0], 0);
			break;
		case 'n':
			config = optarg;
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
		default:
			fprintf(stderr, "Invalid command line parameter\n");
			usage(argv[0], 1);
		}
	if (!prog)
		prog = getenv("SHELL");
	if (!prog)
		prog = "/bin/sh";
	if (login) {
		static char opt[] = "-l";
		argv[--optind] = opt;
	}
	if (check_config) {
		if (drop_privileges())
			exit(2);
		if (config && do_config(config) != 0)
			exit(3);
		printf("# starting '%s'", prog);
		for (; optind < argc; ++optind)
			printf(" '%s'", argv[optind]);
		fputc('\n', stdout);
		exit(0);
	}
	if (unshare(CLONE_NEWNS) == 0) {
		if (mount("none", "/", NULL,
			  MS_REC | (lock_fs ? MS_PRIVATE : MS_SLAVE), NULL) != 0) {
			fprintf(stderr, "%s: setting mount propagation: %s\n",
				argv[0], strerror(errno));
			exit(2);
		}
	} else {
		fprintf(stderr, "%s: unshare(CLONE_NEWNS): %s\n",
			argv[0], strerror(errno));
		exit(2);
	}
	/* FIXME that's a bit careless: reading and parsing with full privileges */
	if (config && do_config(config) != 0)
		exit(3);
	if (drop_privileges())
		exit(2);

	argv[optind - 1] = (char *)prog;
	if (verbose) {
		int i;
		fprintf(stderr, "%s: starting '%s'", build_container, prog);
		for (i = optind; i < argc; ++i)
			fprintf(stderr, " '%s'", argv[i]);
		fputc('\n', stderr);
	}
	execvp(prog, argv + optind - 1);
	fprintf(stderr, "%s: %s\n", prog, strerror(errno));
	return 1;
}

