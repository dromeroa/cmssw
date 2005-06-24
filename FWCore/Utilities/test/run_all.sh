#!/bin/sh

#----------------------------------------------------------------------
# $Id: run_all.sh,v 1.1 2005/06/22 22:22:30 paterno Exp $
#----------------------------------------------------------------------

# Pass in name and status
function die { echo $1: status $2 ;  exit $2; }

# Pass in name
function do_or_die { echo ===== Running $1 ===== && $1 && echo ===== $1 OK ===== || die ">>>>> $1 failed <<<<<" $?; }

do_or_die Simple_t
do_or_die Exception_t
do_or_die ExceptionDerived_t
do_or_die CodedException_t
do_or_die EDMException_t



