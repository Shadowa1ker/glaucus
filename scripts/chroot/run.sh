#!/usr/bin/dash -e

# Copyright (c) 2019, Firas Khalil Khana
# Distributed under the terms of the ISC License

. /home/glaucus/scripts/chroot/variables
. $CSCR/prepare
. $CSCR/log

. $CSCR/check

. $CSCR/fetch
. $CSCR/verify

(. $CSCR/construct | tee $CLOG/$log/out.log) 3>&1 1>&2 2>&3 | tee \
  $CLOG/$log/err.log

. $CSCR/clean
. $CSCR/root
. $CSCR/backup
. $CSCR/vkfs
. $CSCR/chroot
