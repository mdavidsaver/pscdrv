/*************************************************************************\
* Copyright (c) 2014 Brookhaven Science Assoc. as operator of
      Brookhaven National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

#include "psc/devcommon.h"

#include <stdio.h>
#include <arpa/inet.h>

#include <pv/standardField.h>

#include <menuFtype.h>
#include <pvstructinRecord.h>

namespace {

namespace pvd = epics::pvData;

const pvd::StructureConstPtr ptyp = pvd::getStandardField()->scalarArray(pvd::pvDouble, "alarm,timeStamp");

template<int dir>
long init_pvi_record(pvstructinRecord* prec)
{
    assert(prec->inp.type==INST_IO);
    try {
        std::auto_ptr<Priv> priv(new Priv(prec));

        prec->val = ptyp->build();

        parse_link(priv.get(), prec->inp.value.instio.string, dir);

        prec->dpvt = (void*)priv.release();

    }CATCH(init_pvi_record, prec)
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

template<typename T, typename N>
std::tr1::shared_ptr<T>
assign(pvstructinRecord* prec, N& name)
{
    std::tr1::shared_ptr<T> fld(prec->val->getSubFieldT<T>(name));
    prec->chg.set(fld->getFieldOffset());
    return fld;
}

template<typename T>
long read_pvi(pvstructinRecord* prec)
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

        pvd::shared_vector<double> scratch;
        const char *pfrom = &priv->block->data[0];

        const size_t len = priv->block->data.size(); // available # of bytes
        const size_t nelm = len/sizeof(T); // # of elements which can be accepted
        // source step size in bytes, default to element size
        const size_t step = priv->step!=0 ? priv->step : sizeof(T);
        size_t dest = 0; // current dest element

        scratch.resize(nelm);
        double *pto = &scratch[0];

        for(size_t src=priv->offset;
            dest<nelm && src<len;
            dest++, src+=step)
        {
            // endian fix and cast to target value type
            T ival = bytes2val<T>(pfrom + src);
            pto[dest] = ival;
        }

        scratch.resize(dest);

        pvd::PVDoubleArrayPtr arr(prec->val->getSubFieldT<pvd::PVDoubleArray>("value"));
        arr->replace(pvd::freeze(scratch));
        prec->chg.set(arr->getFieldOffset());

        setRecTimestamp(priv);

        // TODO: in record support somehow?
        assign<pvd::PVScalar>(prec, "timeStamp.secondsPastEpoch")->putFrom(prec->time.secPastEpoch + POSIX_TIME_AT_EPICS_EPOCH);
        assign<pvd::PVScalar>(prec, "timeStamp.nanoseconds")->putFrom(prec->time.nsec);
        assign<pvd::PVScalar>(prec, "timeStamp.userTag")->putFrom<pvd::uint64>(prec->utag);

        assign<pvd::PVScalar>(prec, "alarm.severity")->putFrom(prec->nsev);
        assign<pvd::PVScalar>(prec, "alarm.status")->putFrom(prec->nsta);
        assign<pvd::PVScalar>(prec, "alarm.message")->putFrom(std::string(prec->namsg));

    }CATCH(read_pvi, prec)

    return 0;
}

MAKEDSET(pvstructin, devPSCBlockInPVI16, &init_pvi_record<0>, &get_iointr_info, &read_pvi<epicsInt16>);
MAKEDSET(pvstructin, devPSCBlockInPVI32, &init_pvi_record<0>, &get_iointr_info, &read_pvi<epicsInt32>);
MAKEDSET(pvstructin, devPSCBlockInPVIF32, &init_pvi_record<0>, &get_iointr_info, &read_pvi<float>);
MAKEDSET(pvstructin, devPSCBlockInPVIF64, &init_pvi_record<0>, &get_iointr_info, &read_pvi<double>);

} // namespace

#include <epicsExport.h>

epicsExportAddress(dset, devPSCBlockInPVI16);
epicsExportAddress(dset, devPSCBlockInPVI32);
epicsExportAddress(dset, devPSCBlockInPVIF32);
epicsExportAddress(dset, devPSCBlockInPVIF64);
