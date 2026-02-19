//
//  TNLyrMask.c
//  sys-tunnel-core-osx
//
//  Created by Marcos Ortega on 15/3/23.
//

#include "nb/NBFrameworkPch.h"
#include "nb/NBObject.h"
#include "nb/core/NBMemory.h"
#include "nb/core/NBNumParser.h"
#include "nb/core/NBStruct.h"
#include "nb/core/NBThreadCond.h"
#include "nb/core/NBIOLnk.h"
#include "nb/core/NBIOPollster.h"
#include "core/TNBuffs.h"
#include "core/TNLyrMask.h"

//TNLyrMask

typedef struct STTNLyrMaskOpq_ {
    STNBObject              prnt;
    STNBThreadCond          cond;
    STNBStopFlagRef         stopFlag;
    ENTNLyrFlow             flow;
    BOOL                    isPrepared;
    BOOL                    isStarted;
    //lyr
    struct {
        //fromMe
        struct {
            //toUp
            struct {
                STTNLyrLstnr lstnr;
            } toUp;
            //toLwr
            struct {
                //read
                struct {
                    BOOL    isBlocked;      //last action retruned zero results, flag reseteable.
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                    BOOL    isShutNotified; //shutdown notified to upperLyr
                } read;
                //write
                struct {
                    BOOL    isBlocked;      //last action retruned zero results, flag reseteable.
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                    BOOL    isShutNotified; //shutdown notified to lwrLyr
                } write;
            } toLwr;
        } fromMe;
        //toMe
        struct {
            BOOL        lnkIsEnabled;   //link is only allowed to be called during 'nxt.consumeMask' call
            STNBIOLnk   lnk;            //link to expose myself sent to next layer
            //fromUp
            struct {
                //read
                struct {
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                } read;
                //write
                struct {
                    BOOL    isFlushPend;    //flush action was send from upper layer (must be consumed)
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                } write;
            } fromUp;
            //fromLwr
            struct {
                //read
                struct {
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                } read;
                //write
                struct {
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                } write;
            } fromLwr;
        } toMe;
    } lyr;
    //io
    struct {
        //fromLwr
        struct {
            STTNBuffs       buff;    //for read-ops coming from upper-lyr
            UI8             seed;
        } fromLwr;
        //fromUp
        struct {
            STTNBuffs       buff;    //for read-ops coming from upper-lyr
            UI8             seed;
        } fromUp;
    } io;
} STTNLyrMaskOpq;

//

NB_OBJREF_BODY(TNLyrMask, STTNLyrMaskOpq, NBObject)

//

SI32 TNLyrMask_ioRead_(void* dst, const SI32 dstSz, void* usrData); //read data to destination buffer, returns the ammount of bytes read, negative in case of error
SI32 TNLyrMask_ioWrite_(const void* src, const SI32 srcSz, void* usrData); //write data from source buffer, returns the ammount of bytes written, negative in case of error
void TNLyrMask_ioFlush_(void* usrData);     //flush write-data
void TNLyrMask_ioShutdown_(const UI8 mask, void* usrData);  //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrMask_ioClose_(void* usrData);     //close ungracefully

//

void TNLyrMask_initZeroed(STNBObject* obj){
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)obj;
    NBThreadCond_init(&opq->cond);
    opq->stopFlag   = NBStopFlag_alloc(NULL);
    //cfg
    {
        //
    }
    //lyr
    {
        //toMe
        {
            opq->lyr.toMe.lnk.usrData        = opq;
            opq->lyr.toMe.lnk.itf.ioRead     = TNLyrMask_ioRead_;
            opq->lyr.toMe.lnk.itf.ioWrite    = TNLyrMask_ioWrite_;
            opq->lyr.toMe.lnk.itf.ioFlush    = TNLyrMask_ioFlush_;
            opq->lyr.toMe.lnk.itf.ioShutdown = TNLyrMask_ioShutdown_;
            opq->lyr.toMe.lnk.itf.ioClose    = TNLyrMask_ioClose_;
        }
    }
    //net
    {
        TNBuffs_init(&opq->io.fromLwr.buff);
        TNBuffs_init(&opq->io.fromUp.buff);
        //
        TNBuffs_create(&opq->io.fromLwr.buff, TN_CORE_CONN_BUFF_SZ);
        TNBuffs_create(&opq->io.fromUp.buff, TN_CORE_CONN_BUFF_SZ);
    }
}

void TNLyrMask_uninitLocked(STNBObject* obj){
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)obj;
    //activate stop-flag
    NBStopFlag_activate(opq->stopFlag);
    //lyr
    {
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrRelease != NULL){
            (*opq->lyr.fromMe.toUp.lstnr.itf.lyrRelease)(opq->lyr.fromMe.toUp.lstnr.usrParam);
        }
        NBMemory_setZeroSt(opq->lyr.fromMe.toUp.lstnr, STTNLyrLstnr);
    }
    //net
    {
        //buffs
        TNBuffs_release(&opq->io.fromLwr.buff);
        TNBuffs_release(&opq->io.fromUp.buff);
    }
    //stop-flag
    if(NBStopFlag_isSet(opq->stopFlag)){
        NBStopFlag_release(&opq->stopFlag);
        NBStopFlag_null(&opq->stopFlag);
    }
    NBThreadCond_release(&opq->cond);
}

//STTNLyrLstnr
//lyr chain
BOOL TNLyrMask_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData); //set next lyr iterface
//data flow
void TNLyrMask_lyrStart_(void* usrData);
BOOL TNLyrMask_lyrIsRunning_(void* usrData);    //cleanup must wait
void TNLyrMask_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrMask_lyrShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrMask_lyrClose_(void* usrData);
//dbg
void TNLyrMask_lyrConcat_(STNBString* dst, void* usrData);

//

BOOL TNLyrMask_getLyrItf(STTNLyrMaskRef ref, STTNLyrLstnr* dst){
    BOOL r = FALSE;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)ref.opaque; NBASSERT(TNLyrMask_isClass(ref))
    if(dst != NULL){
        NBObject_lock(opq);
        {
            NBMemory_setZeroSt(dst->itf, STTNLyrLstnrItf);
            dst->itf.lyrRetain     = NBObjRef_retainOpq;
            dst->itf.lyrRelease    = NBObjRef_releaseOpq;
            //lyr chain
            dst->itf.lyrSetNext    = TNLyrMask_lyrSetNext_; //set next lyr iterface
            //data flow
            dst->itf.lyrStart       = TNLyrMask_lyrStart_;
            dst->itf.lyrIsRunning   = TNLyrMask_lyrIsRunning_; //cleanup must wait
            dst->itf.lyrConsumeMask = TNLyrMask_lyrConsumeMask_;
            dst->itf.lyrShutdown    = TNLyrMask_lyrShutdown_;
            dst->itf.lyrClose       = TNLyrMask_lyrClose_;
            //dbg
            dst->itf.lyrConcat      = TNLyrMask_lyrConcat_;
            //
            dst->usrParam           = opq;
            r = TRUE;
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrMask_setParentStopFlag(STTNLyrMaskRef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)ref.opaque; NBASSERT(TNLyrMask_isClass(ref))
    NBObject_lock(opq);
    {
        if(NBStopFlag_isSet(opq->stopFlag)){
            NBStopFlag_setParentFlag(opq->stopFlag, parentStopFlag);
            r = TRUE;
        }
    }
    NBObject_unlock(opq);
    return r;
}

//

BOOL TNLyrMask_prepare(STTNLyrMaskRef ref, const ENTNLyrFlow flow, const UI8 seed){
    BOOL r = FALSE;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)ref.opaque; NBASSERT(TNLyrMask_isClass(ref))
    if(flow == ENTNLyrFlow_FromUp || flow == ENTNLyrFlow_FromLwr){
        NBObject_lock(opq);
        if(!opq->isPrepared){
            r = TRUE;
            //apply
            if(r){
                opq->flow = flow;
                opq->isPrepared = TRUE;
                opq->io.fromLwr.seed = seed;
                opq->io.fromUp.seed = seed;
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

UI8 TNLryMask_encode(UI8 seed, void* pData, const UI32 dataSz){ //returns the new seed for next call
    if(pData != NULL && dataSz > 0){
        UI8* data = (UI8*)pData;
        UI8* dataAfterEnd = data + dataSz;
        while(data < dataAfterEnd){
            *data = seed = (UI8)(*data + (UI8)seed);
            data++;
        }
    }
    return seed;
}


UI8 TNLryMask_decode(UI8 seed, void* pData, const UI32 dataSz){    //returns the new seed for next call
    if(pData != NULL && dataSz > 0){
        UI8 tmp = 0;
        UI8* data = (UI8*)pData;
        UI8* dataAfterEnd = data + dataSz;
        while(data < dataAfterEnd){
            *data = (UI8)((tmp = *data) - (UI8)seed);
            seed = tmp;
            data++;
        }
    }
    return seed;
}

//lyr chain

BOOL TNLyrMask_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData){ //set next lyr iterface
    BOOL r = FALSE;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //
    STTNLyrLstnr toRelease;
    NBMemory_setZeroSt(toRelease, STTNLyrLstnr);
    //retain new
    if(nxt != NULL){
        if(nxt->itf.lyrRetain != NULL){
            (*nxt->itf.lyrRetain)(nxt->usrParam);
        }
        toRelease = *nxt;
    }
    NBObject_lock(opq);
    {
        //ToDo: allow only once
        //apply
        toRelease = opq->lyr.fromMe.toUp.lstnr;
        opq->lyr.fromMe.toUp.lstnr = *nxt;
        r = TRUE;
    }
    NBObject_unlock(opq);
    //release
    if(toRelease.itf.lyrRelease != NULL){
        (toRelease.itf.lyrRelease)(toRelease.usrParam);
    }
    NBMemory_setZeroSt(toRelease, STTNLyrLstnr);
    return r;
}

//data flow

void TNLyrMask_lyrStart_(void* usrData){
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    if(opq->isPrepared && !opq->isStarted){
        //upper
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrStart != NULL){
            (*opq->lyr.fromMe.toUp.lstnr.itf.lyrStart)(opq->lyr.fromMe.toUp.lstnr.usrParam);
        }
        opq->isStarted = TRUE;
    }
    NBObject_unlock(opq);
}

BOOL TNLyrMask_lyrIsRunning_(void* usrData){    //cleanup must wait
    BOOL r = FALSE;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        //flushing write data or upper layer isBussy
        r = (
             //my flags
             (opq->isStarted && (!opq->lyr.fromMe.toLwr.read.isShuttedDown || !opq->lyr.fromMe.toLwr.read.isShutNotified || !opq->lyr.fromMe.toLwr.write.isShuttedDown || !opq->lyr.fromMe.toLwr.write.isShutNotified))
             //upper flags
             || (opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning != NULL && (*opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning)(opq->lyr.fromMe.toUp.lstnr.usrParam))
            );
    }
    NBObject_unlock(opq);
    return r;
}

void TNLyrMask_readFromLwrLyrOpq_(STTNLyrMaskOpq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgReadsCount = 0;)
    STTNBuffs* buffs = &opq->io.fromLwr.buff;
    while(!opq->lyr.fromMe.toLwr.read.isBlocked && TNBuffs_canFill(buffs) && !opq->lyr.fromMe.toLwr.read.isShuttedDown){
        UI8* dst            = &buffs->fill->data[buffs->fill->filled];
        const UI32 dstSz    = (buffs->fill->size - buffs->fill->filled);
        const SI32 rr       = (*lnk->itf.ioRead)(dst, dstSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrMask, ioRead failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.read.isBlocked = opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                //PRINTF_INFO("TNLyrMask, (%s) ioRead from down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                switch(opq->flow){
                    case ENTNLyrFlow_FromLwr:
                        //mask
                        opq->io.fromLwr.seed = TNLryMask_encode(opq->io.fromLwr.seed, dst, rr);
                        break;
                    case ENTNLyrFlow_FromUp:
                        //unmask
                        opq->io.fromLwr.seed = TNLryMask_decode(opq->io.fromLwr.seed, dst, rr);
                        break;
                    default:
                        NBASSERT(FALSE); //unexpected flow
                        break;
                }
                TNBuffs_moveFillCursor(buffs, rr);
            }
            opq->lyr.fromMe.toLwr.read.isBlocked = (rr < dstSz);
        }
        IF_NBASSERT(dbgReadsCount++;)
        IF_NBASSERT(if(dbgReadsCount > 3){ PRINTF_WARNING("TNLyrMask, %d reads.\n", dbgReadsCount); })
    }
}

void TNLyrMask_writeToLwrLyrOpq_(STTNLyrMaskOpq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgWritesCount = 0;)
    STTNBuffs* buffs = &opq->io.fromUp.buff;
    while(TNBuffs_canConsume(buffs) && !opq->lyr.fromMe.toLwr.write.isBlocked && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        const void* src     = &buffs->read->data[buffs->read->csmd];
        const UI32 srcSz    = buffs->read->filled - buffs->read->csmd;
        const SI32 rr       = (*lnk->itf.ioWrite)(src, srcSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrMask, ioWrite failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.write.isBlocked = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                //PRINTF_INFO("TNLyrMask, (%s) ioWrite to down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                //data should be already masked/unmasked
                TNBuffs_moveCsmCursor(buffs, rr);
            }
            opq->lyr.fromMe.toLwr.write.isBlocked = (rr < srcSz);
        }
        IF_NBASSERT(dbgWritesCount++;)
        IF_NBASSERT(if(dbgWritesCount > 3){ PRINTF_WARNING("TNLyrMask, %d writes.\n", dbgWritesCount); })
    }
    //flush
    if(!TNBuffs_canConsume(buffs) && opq->lyr.toMe.fromUp.write.isFlushPend && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        opq->lyr.toMe.fromUp.write.isFlushPend = FALSE;
        if(lnk->itf.ioFlush != NULL){
            (*lnk->itf.ioFlush)(lnk->usrData);
        }
    }
}
    
void TNLyrMask_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData){
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        UI8 pollReqNxt = 0;
        //
        if(pollMask & ENNBIOPollsterOpBit_Read){
            opq->lyr.fromMe.toLwr.read.isBlocked = FALSE;
        }
        if(pollMask & ENNBIOPollsterOpBit_Write){
            opq->lyr.fromMe.toLwr.write.isBlocked = FALSE;
        }
        //read from lower-lyr
        TNLyrMask_readFromLwrLyrOpq_(opq, lnk);
        //write to lower-lyr
        TNLyrMask_writeToLwrLyrOpq_(opq, lnk);
        //push to nxt-lyr
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask != NULL){
            NBASSERT(!opq->lyr.toMe.lnkIsEnabled)
            opq->lyr.toMe.lnkIsEnabled = TRUE;
            {
                pollReqNxt = 0;
                const UI8 pollMaskNxt = (TNBuffs_canConsume(&opq->io.fromLwr.buff) ? ENNBIOPollsterOpBit_Read : 0) | (TNBuffs_canFill(&opq->io.fromUp.buff) ? ENNBIOPollsterOpBit_Write : 0);
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask)(&opq->lyr.toMe.lnk, pollMaskNxt, &pollReqNxt, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
            opq->lyr.toMe.lnkIsEnabled = FALSE;
        }
        //apply stopFlag (inmediatly)
        if(NBStopFlag_isAnyActivated(opq->stopFlag)){
            opq->lyr.fromMe.toLwr.read.isShuttedDown = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
        }
        //apply upper/lwr shutdowns to me (after consumeMask)
        {
            if(!opq->lyr.fromMe.toLwr.read.isShuttedDown && ((opq->lyr.toMe.fromLwr.read.isShuttedDown && !TNBuffs_canConsume(&opq->io.fromLwr.buff) /*can flush*/) || opq->lyr.toMe.fromUp.read.isShuttedDown /*inmediate*/)){
                //shutdown my read
                //PRINTF_INFO("TNLyrMask, shutting read after %s layer's.\n", opq->lyr.toMe.fromUp.read.isShuttedDown ? "upper" : "lwr");
                opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            }
            //
            if(!opq->lyr.fromMe.toLwr.write.isShuttedDown && (opq->lyr.toMe.fromLwr.write.isShuttedDown /*inmediate*/ || (opq->lyr.toMe.fromUp.write.isShuttedDown && !TNBuffs_canConsume(&opq->io.fromUp.buff) /*can flush*/))){
                //shutdown my write
                //PRINTF_INFO("TNLyrMask, shutting write after %s layer's.\n", opq->lyr.toMe.fromLwr.write.isShuttedDown ? "lwr" : "upper");
                opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
            }
        }
        //notify my shutdowns to upLyr and lwrLyr (after consumeMask)
        {
            UI8 shutMaskUp = 0, shutMaskLwr = 0;
            if(!opq->lyr.fromMe.toLwr.read.isShutNotified && opq->lyr.fromMe.toLwr.read.isShuttedDown){
                opq->lyr.fromMe.toLwr.read.isShutNotified = TRUE;
                if(!opq->lyr.toMe.fromLwr.read.isShuttedDown){ //no need to notify if lwr layer already shutted down
                    shutMaskLwr |= NB_IO_BIT_READ;
                }
                if(!opq->lyr.toMe.fromUp.read.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_READ;
                }
            }
            if(!opq->lyr.fromMe.toLwr.write.isShutNotified && opq->lyr.fromMe.toLwr.write.isShuttedDown){
                opq->lyr.fromMe.toLwr.write.isShutNotified = TRUE;
                if(!opq->lyr.toMe.fromLwr.write.isShuttedDown){ //no need to notify if lwr layer already shutted down
                    shutMaskLwr |= NB_IO_BIT_WRITE;
                }
                if(!opq->lyr.toMe.fromUp.write.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_WRITE;
                }
            }
            IF_PRINTF(
                      if((shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL) || shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL){
                          //PRINTF_INFO("TNLyrMask, notifying %s%s%s%s %s%s%s%s.\n", (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? "lwrLyr-shut(" : ""), (shutMaskLwr & NB_IO_BIT_READ ? "rd" : ""), (shutMaskLwr & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? ")" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? "upLyr-shut(" : ""), (shutMaskUp & NB_IO_BIT_READ ? "rd" : ""), (shutMaskUp & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? ")" : ""));
                      }
            )
            if(shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL){
                (*lnk->itf.ioShutdown)(shutMaskLwr, lnk->usrData);
            }
            if(shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL){
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown)(shutMaskUp, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
        }
        //
        if(dstPollReq != NULL){
            *dstPollReq = pollReqNxt;
        }
        //bussy?
        {
            //const BOOL imBussy = (!opq->lyr.fromMe.toLwr.read.isShuttedDown || !opq->lyr.fromMe.toLwr.read.isShutNotified || !opq->lyr.fromMe.toLwr.write.isShuttedDown || !opq->lyr.fromMe.toLwr.write.isShutNotified);
            //if(!imBussy){
            //    PRINTF_INFO("TNLyrMask, not-bussy.\n");
            //}
        }
    }
    NBObject_unlock(opq);
}

void TNLyrMask_lyrShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        //lower layer is notifying that these actions will error if called again
        if(mask & NB_IO_BIT_READ){
            opq->lyr.toMe.fromLwr.read.isShuttedDown = TRUE;
        }
        if(mask & NB_IO_BIT_WRITE){
            opq->lyr.toMe.fromLwr.write.isShuttedDown = TRUE;
        }
    }
    NBObject_unlock(opq);
}

void TNLyrMask_lyrClose_(void* usrData){
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        opq->lyr.toMe.fromLwr.read.isShuttedDown = opq->lyr.toMe.fromLwr.write.isShuttedDown = TRUE;
    }
    NBObject_unlock(opq);
}

void TNLyrMask_lyrConcat_(STNBString* dst, void* usrData) {
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    if (dst != NULL) {
        NBObject_lock(opq);
        {
            NBString_concat(dst, "TNLyrMsk (");
            NBString_concat(dst, (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
            NBString_concat(dst, ") r("); NBString_concat(dst, opq->lyr.fromMe.toLwr.read.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ") w("); NBString_concat(dst, opq->lyr.fromMe.toLwr.write.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ")\n");
            if (opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask == NULL) {
                NBString_concat(dst, "end-of-lyrs\n");
            } else if (opq->lyr.fromMe.toUp.lstnr.itf.lyrConcat == NULL) {
                NBString_concat(dst, "upper-lyrs-missing-lyrConcat\n");
            } else {
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrConcat)(dst, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
        }
        NBObject_unlock(opq);
    }
}

//io

SI32 TNLyrMask_ioRead_(void* pDst, const SI32 dstSz, void* usrData) { //read data to destination buffer, returns the ammount of bytes read, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.read.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be reading anymore.
        } else if(dstSz == 0){
            r = 0;
        } else if(dstSz > 0){
            STTNBuffs* buffs = &opq->io.fromLwr.buff;
            //Data should be already masked/unmasked
            r = TNBuffs_consume(buffs, pDst, dstSz);
            if(r == 0 && opq->lyr.fromMe.toLwr.read.isShuttedDown){
                //ERROR or EOF
                PRINTF_INFO("TNLyrMask, ioRead sending ERROR-EOF-sginal.\n");
                r = NB_IO_ERROR;
            }
        }
    }
    return r;
}

SI32 TNLyrMask_ioWrite_(const void* pSrc, const SI32 srcSz, void* usrData){ //write data from source buffer, returns the ammount of bytes written, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.write.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be writting anymore.
        } else if(srcSz == 0){
            r = 0;
        } else if(srcSz > 0){
            if(opq->lyr.toMe.fromUp.write.isFlushPend){
                //do not fill write buffer untill flushed.
                r = 0;
            } else {
                STTNBuffs* buffs = &opq->io.fromUp.buff;
                if(pSrc != NULL && buffs->fill != NULL){
                    const BYTE* src = (const BYTE*)pSrc;
                    r = 0;
                    while(r < srcSz && buffs->fill->filled < buffs->fill->size){
                        UI8* dst                = (UI8*)&buffs->fill->data[buffs->fill->filled];
                        const SI32 srcAvail     = (srcSz - r);
                        const SI32 fillAvail    = (buffs->fill->size - buffs->fill->filled);
                        const SI32 moveSz       = (srcAvail < fillAvail ? srcAvail : fillAvail);
                        NBASSERT(moveSz > 0)
                        NBMemory_copy(dst, &src[r], moveSz);
                        r += moveSz;
                        //move cursor
                        switch(opq->flow){
                            case ENTNLyrFlow_FromLwr:
                                //unmask
                                opq->io.fromUp.seed = TNLryMask_decode(opq->io.fromUp.seed, dst, moveSz);
                                break;
                            case ENTNLyrFlow_FromUp:
                                //mask
                                opq->io.fromUp.seed = TNLryMask_encode(opq->io.fromUp.seed, dst, moveSz);
                                break;
                            default:
                                NBASSERT(FALSE); //unexpected flow
                                break;
                        }
                        NBASSERT(moveSz > 0)
                        TNBuffs_moveFillCursor(buffs, moveSz);
                    }
                }
            }
        }
    }
    return r;
}

void TNLyrMask_ioFlush_(void* usrData){ //flush write-data
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.write.isFlushPend = TRUE;
    }
}

void TNLyrMask_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(mask & NB_IO_BIT_READ){
            opq->lyr.toMe.fromUp.read.isShuttedDown = TRUE;
        }
        if(mask & NB_IO_BIT_WRITE){
            opq->lyr.toMe.fromUp.write.isShuttedDown = TRUE;
        }
    }
}

void TNLyrMask_ioClose_(void* usrData){ //close ungracefully
    STTNLyrMaskOpq* opq = (STTNLyrMaskOpq*)usrData; NBASSERT(TNLyrMask_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.read.isShuttedDown = opq->lyr.toMe.fromUp.write.isShuttedDown = TRUE;
    }
}
