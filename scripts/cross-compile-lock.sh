#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# cross-compile-lock.sh — RETIRED no-op pass-through (prestonbrown/helixscreen#1058)
#
# Historically this serialized cross-compile docker runs with an atomic mkdir
# lock on /tmp/helixscreen-cross-compile.lock, because libhv compiled its
# objects IN-TREE (lib/libhv/*.o and lib/libhv/lib/libhv.a) and two builds for
# different architectures would clobber each other (see #766: concurrent
# pi-docker and ad5m-docker stomping on lib/libhv artifacts).
#
# As of #1058 libhv's Makefile.in honors an OBJDIR/LIBDIR override, and
# mk/deps.mk points each arch at its own out-of-tree object dir
# (build/libhv-objs, build/pi/libhv-objs, …). Cross builds no longer share any
# in-tree artifact, so the mutual exclusion is unnecessary — concurrent
# cross-compiles are safe.
#
# The script is kept as a transparent pass-through (rather than deleted) so the
# ~20 docker-run call sites in mk/cross.mk continue to work unchanged. It simply
# execs the wrapped command with no locking.
#
# Usage (unchanged):
#     scripts/cross-compile-lock.sh docker run --rm ...

exec "$@"
