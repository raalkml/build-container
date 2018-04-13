# build-container

A container implementation for software builds on Linux systems.

Just unshares the filesystem namespace and allows a bind, move and an overlays.
Obviously requires root set uid or sudo/su (CAP_SYSADMIN).
