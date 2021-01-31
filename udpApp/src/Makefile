TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

USR_CPPFLAGS += -I$(TOP)/coreApp/src
USR_CPPFLAGS += -D_FILE_OFFSET_BITS=64

DBD += pscudp.dbd

LIBRARY += pscudp

# library uses features recvmmsg() not found in winsock, RTEMS <5, or (likely) vxWorks
# not likely to find a 10G NIC with RTEMS/vxWorks anyway
pscudp_SRCS_DEFAULT += udpdrv.cpp
pscudp_SRCS_WIN32 += dummy.c
pscudp_SRCS_RTEMS += dummy.c
pscudp_SRCS_vxWorks += dummy.c

pscudp_LIBS += $(EPICS_BASE_IOC_LIBS)

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

