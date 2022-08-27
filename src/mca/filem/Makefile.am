#
# Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
noinst_LTLIBRARIES = libmca_filem.la
libmca_filem_la_SOURCES =

# local files
headers = filem.h
libmca_filem_la_SOURCES += $(headers)

# Conditionally install the header files
if WANT_INSTALL_HEADERS
prtedir = $(prteincludedir)/$(subdir)
nobase_prte_HEADERS = $(headers)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
