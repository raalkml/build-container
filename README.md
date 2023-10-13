# build-container

*This is not a containerization as provided by Docker, Podman, or similar!*

This project provides some isolation for software builds and software testing for Linux systems.

The program allows unsharing of the filesystem and networking namespaces and allows a chroot, bind, move, r/w overlay and r/o union. It can also setup a user namespace for root-less operation (as [RootlessKit](https://github.com/rootless-containers/rootlesskit)) or use a SUID root or sudo/su (to get `CAP_SYSADMIN`).

See man-pages for `mount(1)`, `mount(2)`, `unshare(2)`, `namespaces(7)` for operational details.

# Example of the configuration file

The configuration file is given with `-n` command-line option.
The arguments of `from`, `to`, and `work` commands, if not absolute, are
relative to the configuration file.

```
# A bind mount, with mount(2) options
from /bin
to my-container-dir/tmp-bin
bind ro rec

# A mountpoint move
from my-container-dir/tmp-bin
to my-container-dir/bin
move

# An r/o union
# The order of `from` lines is meaningful! (top-down)
# More than two `from` lines allowed.
from t/union-top
from t/union-bottom
to t/merged
union xino=off index=off ro

# An r/w overlay
# Exactly two `from`, one `work` and one `to` lines!
from t/top
from t/bottom
work t/wrk
to t/merged
overlay xino=auto index=off

# An r/o overlay
from t/top
from t/bottom
work t/wrk
to t/merged
overlay xino=auto index=off ro

# chroot(2)
chroot t/merged

# A tmpfs mount, with options
to t/runtime
mount tmpfs rw

# A loopback device mount
from artifacts.squashfs
to t
mount squashfs loop ro

```
