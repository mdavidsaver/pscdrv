TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

LIBRARY_IOC = pscCore

DBD += pscCore.dbd

pscCore_SRCS += evbase.cpp
pscCore_SRCS += pscbase.cpp
pscCore_SRCS += psc.cpp
pscCore_SRCS += pscudp.cpp
pscCore_SRCS += pscwrap.cpp
pscCore_SRCS += util.c
pscCore_SRCS += devcommon.cpp
pscCore_SRCS += devPSCString.cpp
pscCore_SRCS += devPSCWf.cpp
pscCore_SRCS += devPSCReg.cpp
pscCore_SRCS += devPSCSingle.cpp
pscCore_SRCS += devPSCCtrl.cpp

pscCore_SYS_LIBS += event_core event_extra
pscCore_SYS_LIBS_Linux += event_pthreads

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

