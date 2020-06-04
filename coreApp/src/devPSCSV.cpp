/*************************************************************************\
* Copyright (c) 2014 Brookhaven Science Assoc. as operator of
      Brookhaven National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include "psc/devcommon.h"

#include <stdio.h>
#include <arpa/inet.h>

#include <menuFtype.h>
#include <svectorinRecord.h>

namespace {

template<int dir>
long init_svi_record(svectorinRecord* prec)
{
    assert(prec->inp.type==INST_IO);
    if(prec->ftvl!=menuFtypeDOUBLE) {
        timefprintf(stderr, "%s: FTVL must be DOUBLE\n", prec->name);
        return 0;
    }
    try {
        std::auto_ptr<Priv> priv(new Priv(prec));

        parse_link(priv.get(), prec->inp.value.instio.string, dir);

        prec->dpvt = (void*)priv.release();

    }CATCH(init_svi_record, prec)
return 0;
}

template<int dir>
long init_svi_record_bytes(svectorinRecord* prec)
{
    assert(prec->inp.type==INST_IO);
    if(!(prec->ftvl==menuFtypeCHAR || prec->ftvl==menuFtypeUCHAR)) {
        timefprintf(stderr, "%s: FTVL must be CHAR or UCHAR\n", prec->name);
        return 0;
    }
    try {
        std::auto_ptr<Priv> priv(new Priv(prec));

        parse_link(priv.get(), prec->inp.value.instio.string, dir);

        prec->dpvt = (void*)priv.release();

    }CATCH(init_svi_record, prec)
return 0;
}

long get_iointr_info(int cmd, dbCommon *prec, IOSCANPVT *io)
{
    if(!prec->dpvt)
        return -1;
    Priv *priv=(Priv*)prec->dpvt;

    *io = priv->block->scan;
    return 0;
}

template<typename T>
long read_svi(svectorinRecord* prec)
{
    if(!prec->dpvt)
        return -1;
    Priv *priv=(Priv*)prec->dpvt;

    try {
        Guard g(priv->psc->lock);

        if(!priv->psc->isConnected()) {
            int junk = recGblSetSevrMsg(prec, READ_ALARM, INVALID_ALARM, "No Conn");
            junk += 1;
            return 0;
        }

        epics::pvData::shared_vector<double> scratch(prec->nelm);
        double *pto = &scratch[0];
        const char *pfrom = &priv->block->data[0];

        const size_t len = priv->block->data.size(); // available # of bytes
        const size_t nelm = prec->nelm; // # of elements which can be accepted
        // source step size in bytes, default to element size
        const size_t step = priv->step!=0 ? priv->step : sizeof(T);
        size_t dest = 0; // current dest element

        for(size_t src=priv->offset;
            dest<nelm && src<len;
            dest++, src+=step)
        {
            // endian fix and cast to target value type
            T ival = bytes2val<T>(pfrom + src);
            pto[dest] = ival;
        }

        scratch.resize(dest);
        prec->val = epics::pvData::static_shared_vector_cast<const void>(epics::pvData::freeze(scratch));

        setRecTimestamp(priv);
    }CATCH(read_svi, prec)

    return 0;
}

long read_svi_bytes(svectorinRecord* prec)
{
    if(!prec->dpvt)
        return -1;
    Priv *priv=(Priv*)prec->dpvt;

    try {
        Guard g(priv->psc->lock);

        if(!priv->psc->isConnected()) {
            int junk = recGblSetSevrMsg(prec, READ_ALARM, INVALID_ALARM, "No Conn");
            junk += 1;
            return 0;
        }

        epics::pvData::shared_vector<char> scratch(prec->nelm);
        char *pto = (char*)&scratch[0];
        const char *pfrom = &priv->block->data[0];

        size_t len = priv->block->data.size();
        if(len>=prec->nelm)
            len=prec->nelm;

        memcpy(pto, pfrom, len);

        scratch.resize(len);
        prec->val = epics::pvData::static_shared_vector_cast<const void>(epics::pvData::freeze(scratch));

        setRecTimestamp(priv);
    }CATCH(read_svi_bytes, prec)

    return 0;
}

MAKEDSET(svectorin, devPSCBlockInSVI8, &init_svi_record_bytes<0>, &get_iointr_info, &read_svi_bytes);
MAKEDSET(svectorin, devPSCBlockInSVI16, &init_svi_record<0>, &get_iointr_info, &read_svi<epicsInt16>);
MAKEDSET(svectorin, devPSCBlockInSVI32, &init_svi_record<0>, &get_iointr_info, &read_svi<epicsInt32>);
MAKEDSET(svectorin, devPSCBlockInSVIF32, &init_svi_record<0>, &get_iointr_info, &read_svi<float>);
MAKEDSET(svectorin, devPSCBlockInSVIF64, &init_svi_record<0>, &get_iointr_info, &read_svi<double>);

} // namespace

#include <epicsExport.h>

epicsExportAddress(dset, devPSCBlockInSVI8);
epicsExportAddress(dset, devPSCBlockInSVI16);
epicsExportAddress(dset, devPSCBlockInSVI32);
epicsExportAddress(dset, devPSCBlockInSVIF32);
epicsExportAddress(dset, devPSCBlockInSVIF64);
