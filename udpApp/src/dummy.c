#include <epicsExport.h>

static
int PSCUDPDebug;

static
void udppsc(void) {}

epicsExportRegistrar(udppsc);
epicsExportAddress(int, PSCUDPDebug);
