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
static char default_overlay_opts[] = "index=off,xino=off,";
static char default_union_opts[] = "xino=off,";
static char *overlay_opts = default_overlay_opts;
static char *union_opts = default_union_opts;

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
static char spaces_lf[] = "\x20\t\r\n";
static char empty_str[] = "";

static int at_line_terminator(const char *s)
{
	return '\n' == *s || !*s;
}

static int at_id_terminator(const char *s)
{
	return strchr(spaces_lf, *s) || at_line_terminator(s);
}

static int is_absolute(const char *path)
{
	return path && '/' == *path;
}

static char *strend(const char *s)
{
	if (s)
		s += strlen(s);
	return (char *)s;
}
static char *strlast(const char *s)
{
	char *e = strend(s);
	if (e > s)
		--e;
	return e;
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

struct dict_element
{
	const char *key;
	unsigned long flags;
};

static const struct dict_element generic_mount_opts[] = {
	{ "rec", MS_REC },
	{ "noexec", MS_NOEXEC },
	{ "nosuid", MS_NOSUID },
	{ "nodev", MS_NODEV },
	{ "ro", MS_RDONLY },
	{ "rw", 0 },
	{ NULL }
};

static inline int is_delimiter(const char *s)
{
	return *s && strchr(spaces_lf, *s);
}

static inline void swap_char(char *a, char *b)
{
	char t = *b;
	*b = *a;
	*a = t;
}

static inline void reverse(char *s, char *e)
{
	if (e - s < 2)
		return;
	while (s < --e)
		swap_char(e, s++);
}

static int in_dictionary(const struct dict_element *dictionary, const char *word, int len)
{
	if (!len)
		return 0;
	while (dictionary->key) {
		if (strncmp(dictionary->key, word, len) == 0)
			return 1;
		++dictionary;
	}
	return 0;
}

static void split_args(char *buffer, const struct dict_element *dictionary, char **k, char **u)
{
	char *buffer_end = buffer + strlen(buffer);
	char *b = buffer;
	char *e = buffer_end;

	*k = e;
	*u = e;
	while (b < e) {
		/* select a word */
		char *ab = b, *ae, *ne;
		for (ae = ab; ae < e && !is_delimiter(ae);)
			++ae;
		/* find the trailing delimiters */
		for (ne = ae; ne < e && is_delimiter(ne);)
			++ne;
		if (ab == ae) { /* empty word (string starting with delimiters) */
			b = ne;
			continue;
		}
		if (in_dictionary(dictionary, ab, ae - ab)) {
			/* known word, leave it and its delimiters in place */
			b = ne;
			continue;
		}
		/* move the word to the end of buffer */
		int len = ne - ab;
		reverse(ab, e);
		reverse(e - len + (ne - ae), e);
		reverse(ab, e - len);
		b = ab;
		e -= len;
	}
	if (e > buffer)
		*k = buffer;
	/* finalize: terminate the known words and reverse the others */
	if (e != buffer_end)
		while (e > buffer) {
			if (is_delimiter(e)) {
				*e++ = '\0';
				break;
			}
			--e;
		}
	*u = e;
	/*
	 * Note: can finish here if the order of unknown words is not
	 * important.
	 */
	b = e;
	reverse(b, buffer_end);
	while (b < buffer_end) {
		char *ab = b;
		while (ab < buffer_end && is_delimiter(ab))
			++ab;
		if (ab == buffer_end)
			break;
		char *ae = ab;
		while (ae < buffer_end && !is_delimiter(ae))
			++ae;
		reverse(ab, ae);
		b = ae;
	}
}

static void args_to_mount_data(char *args)
{
	char *o = args;
	args += strspn(args, spaces_lf);
	while (*args) {
		if (strchr(spaces_lf, *args)) {
			args += strspn(args, spaces_lf);
			*o++ = ',';
			continue;
		}
		*o++ = *args++;
	}
	*o = '\0';
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
	if (*strlast(abspath_buf) != '/')
		strcat(abspath_buf, "/");
	strcat(abspath_buf, name);
	return abspath_buf;
}

static int do_mount_options(unsigned long *opts, char *arg)
{
	arg = cleanup(arg);
	while (!at_line_terminator(arg)) {
		unsigned i;
		arg += strspn(arg, spaces);
		if (!*arg)
			break;
		for (i = 0; generic_mount_opts[i].key; ++i)
			if (expect_id(generic_mount_opts[i].key, &arg)) {
				*opts |= generic_mount_opts[i].flags;
				break;
			}
		if (!generic_mount_opts[i].key) {
			error("syntax error: mount option not supported: %s\n", arg);
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
		printf("# mount '%s' '%s' %s 0x%lx%s '%s'\n",
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
		const char *e = strend(file);

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
		free(file);
		if (fp)
			break;
		p = next;
	}
	free(dirs);
	return fp;
}

static int do_config_bind(struct stk **head, char *arg)
{
	int ret;
	struct stk *b = pop(head);
	struct stk *a = pop(head);
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
	return ret;
}

static int do_config_move(struct stk **head, char *arg)
{
	int ret;
	struct stk *b = pop(head);
	struct stk *a = pop(head);
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
	return ret;
}

static int do_config_union(struct stk **head, char *arg)
{
	int ret = 0;
	struct stk *a = NULL, *b = NULL, *e;
	/* collect all 'from' paths and exactly one 'to' */
	size_t lowersize = 0;

	while (*head) {
		switch ((*head)->arg) {
		case FROM:
			e = pop(head);
			lowersize += strlen(e->val) + 1;
			e->next = a;
			a = e;
			break;
		case TO:
			if (b)
				ret = -2;
			else
				b = pop(head);
		default:
			break;
		}
	}
	if (ret == -2 || !a || !b) {
		ret = -1;
		error("'union' expects exactly one 'to' path "
		      "and at least one from\n");
	} else {
		char *data, *mnt_opts = empty_str, *ovl_opts = empty_str;
		arg = cleanup(arg);
		split_args(arg, generic_mount_opts, &mnt_opts, &ovl_opts);
		if (*ovl_opts)
			args_to_mount_data(ovl_opts);
		else
			ovl_opts = union_opts;
		data = malloc(strlen(ovl_opts) + 1 + sizeof("lowerdir") + lowersize);
		strcpy(data, ovl_opts);
		strcat(data, ",lowerdir=" + (!*data || *strlast(data) == ','));
		for (e = a; e; e = e->next) {
			strcat(data, e->val);
			if (e->next)
				strcat(data, ":");
		}
		ret = do_mount("union", b->val, "overlay", 0, data, mnt_opts);
		free(data);
	}
	free(b);
	while (a) {
		e = a->next;
		free(a);
		a = e;
	}
	return ret;
}

static int do_config_overlay(struct stk **head, char *arg)
{
	int ret = 0;
	struct stk *a = NULL, *b = NULL, *w = NULL, *e;
	/* collect one 'work', two 'from' paths, and exactly one 'to' */
	while (*head && ret == 0 && !(a && a->next && b && w)) {
		switch ((*head)->arg) {
		case WORK:
			if (w)
				ret = -2;
			else
				w = pop(head);
			break;
		case FROM:
			if (a && a->next)
				ret = -2;
			else {
				e = pop(head);
				e->next = a;
				a = e;
			}
			break;
		case TO:
			if (b)
				ret = -2;
			else
				b = pop(head);
			break;
		}
	}
	if (ret == -2 || !(a && a->next) || !b || !w) {
		ret = -1;
		error("'overlay' expects exactly one 'work', two 'from', "
		      "and one 'to' path lines\n");
	} else {
		char *data, *mnt_opts = empty_str, *ovl_opts = empty_str;
		arg = cleanup(arg);
		split_args(arg, generic_mount_opts, &mnt_opts, &ovl_opts);
		if (*ovl_opts)
			args_to_mount_data(ovl_opts);
		else
			ovl_opts = overlay_opts;
		data = malloc(strlen(ovl_opts) +
			      sizeof("lowerdir=,upperdir=,workdir=") +
			      strlen(a->val) + strlen(a->next->val) +
			      strlen(b->val) + strlen(w->val));
		sprintf(data, "%s%supperdir=%s,lowerdir=%s,workdir=%s",
			ovl_opts,
			"," + (!*ovl_opts || *strlast(ovl_opts) == ','),
			a->val, a->next->val, w->val);
		ret = do_mount("overlay", b->val, "overlay", 0, data, mnt_opts);
		free(data);
	}
	free(w);
	free(b);
	while (a) {
		e = a->next;
		free(a);
		a = e;
	}
	return ret;
}

static int do_config(const char *config)
{
	char line[BUFSIZ];
	struct stk *head = NULL;
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
		else if (expect_id("bind", &arg))
			ret = do_config_bind(&head, arg);
		else if (expect_id("move", &arg))
			ret = do_config_move(&head, arg);
		else if (expect_id("union", &arg))
			ret = do_config_union(&head, arg);
		else if (expect_id("overlay", &arg))
			ret = do_config_overlay(&head, arg);
		else if (expect_id("chroot", &arg))
			ret = do_chroot(abspath(config_dir, cleanup(arg)));
		if (ret)
			break;
	}
	fclose(fp);
	free(config_dir);
	while (head) {
		struct stk *a = head->next;
		free(head);
		head = a;
	}
	free(abspath_buf);
	abspath_buf = NULL;
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

#define PIDNS_OWN_PROC 1

static int run_pidns_container(const char *cd_to, unsigned flags, const char *prog, char **argv)
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
		if ((flags & PIDNS_OWN_PROC) && mount("proc", "/proc", "proc", 0, NULL) != 0) {
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
		return run_pidns_container(cd_to,
					   pidns > 1 ? PIDNS_OWN_PROC : 0,
					   prog, argv + optind - 1);
	return run_container(cd_to, prog, argv + optind - 1);
}

