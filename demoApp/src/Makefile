TOP=../..

include $(TOP)/configure/CONFIG
#----------------------------------------
#  ADD MACRO DEFINITIONS AFTER THIS LINE
#=============================

#=============================
# Build the IOC application

PROD_IOC = pscdemo
# demo.dbd will be created and installed
DBD += pscdemo.dbd

# demo.dbd will be made up from these files:
pscdemo_DBD += base.dbd
pscdemo_DBD += system.dbd
pscdemo_DBD += pscCore.dbd

ifdef EPICS_QSRV_MAJOR_VERSION
pscdemo_DBD += PVAServerRegister.dbd
pscdemo_DBD += qsrv.dbd
pscdemo_LIBS += qsrv
pscdemo_LIBS += $(EPICS_BASE_PVA_CORE_LIBS)
endif

ifdef AUTOSAVE
pscdemo_DBD += asSupport.dbd
pscdemo_LIBS += autosave
endif

pscdemo_DBD += pscUDPFast.dbd
pscdemo_LIBS += pscUDPFast

pscdemo_LIBS += pscCore

# pscdemo_registerRecordDeviceDriver.cpp derives from pscdemo.dbd
pscdemo_SRCS += pscdemo_registerRecordDeviceDriver.cpp

# Build the main IOC entry point on workstation OSs.
pscdemo_SRCS_DEFAULT += demoMain.cpp
pscdemo_SRCS_vxWorks += -nil-

# Finally link to the EPICS Base libraries
pscdemo_LIBS += $(EPICS_BASE_IOC_LIBS)

pscdemo_SYS_LIBS += event_core event_extra
pscdemo_SYS_LIBS_DEFAULT = event_pthreads
pscdemo_SYS_LIBS_WIN32 =

#===========================

include $(TOP)/configure/RULES
#----------------------------------------
#  ADD RULES AFTER THIS LINE

