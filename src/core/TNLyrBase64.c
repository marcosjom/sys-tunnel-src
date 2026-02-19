//
//  TNLyrBase64.c
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
#include "nb/crypto/NBBase64.h"
#include "core/TNBuffs.h"
#include "core/TNLyrBase64.h"

//TNLyrBase64

typedef struct STTNLyrBase64Opq_ {
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
                    BOOL    isShutNotified; //shutdown notified to upperLyr
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
                    BOOL    isFlushCalc;    //flush action was applied into buffer.
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
        //fromLwr //for read-ops coming from upper-lyr
        struct {
            STTNBuffs   raw;
            STTNBuffs   proc;
        } fromLwr;
        //fromUp //for write-ops coming from upper-lyr
        struct {
            STTNBuffs   raw;
            STTNBuffs   proc;
        } fromUp;
    } io;
} STTNLyrBase64Opq;

//

NB_OBJREF_BODY(TNLyrBase64, STTNLyrBase64Opq, NBObject)

#define TNLyrBase64_needFlushIncoming(OPQ)      (TNBuffs_canConsume(&opq->io.fromUp.raw) && ((OPQ)->lyr.toMe.fromUp.write.isShuttedDown || ((OPQ)->lyr.toMe.fromUp.write.isFlushPend && !(OPQ)->lyr.toMe.fromUp.write.isFlushCalc)))
#define TNLyrBase64_needFlushOutgoing(OPQ)      (TNBuffs_canConsume(&opq->io.fromLwr.raw) && (((OPQ)->lyr.toMe.fromLwr.read.isShuttedDown || (OPQ)->lyr.fromMe.toLwr.read.isShuttedDown) && !(OPQ)->lyr.toMe.fromUp.read.isShuttedDown))
#define TNLyrBase64_needFlush(OPQ)              ((OPQ)->flow == ENTNLyrFlow_FromUp && TNLyrBase64_needFlushIncoming(OPQ) || (OPQ)->flow == ENTNLyrFlow_FromLwr && TNLyrBase64_needFlushOutgoing(OPQ))

//

SI32 TNLyrBase64_ioRead_(void* dst, const SI32 dstSz, void* usrData); //read data to destination buffer, returns the ammount of bytes read, negative in case of error
SI32 TNLyrBase64_ioWrite_(const void* src, const SI32 srcSz, void* usrData); //write data from source buffer, returns the ammount of bytes written, negative in case of error
void TNLyrBase64_ioFlush_(void* usrData);       //flush write-data
void TNLyrBase64_ioShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrBase64_ioClose_(void* usrData);       //close ungracefully

//

void TNLyrBase64_initZeroed(STNBObject* obj){
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)obj;
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
            opq->lyr.toMe.lnk.itf.ioRead     = TNLyrBase64_ioRead_;
            opq->lyr.toMe.lnk.itf.ioWrite    = TNLyrBase64_ioWrite_;
            opq->lyr.toMe.lnk.itf.ioFlush    = TNLyrBase64_ioFlush_;
            opq->lyr.toMe.lnk.itf.ioShutdown = TNLyrBase64_ioShutdown_;
            opq->lyr.toMe.lnk.itf.ioClose    = TNLyrBase64_ioClose_;
        }
    }
    //net
    {
        TNBuffs_init(&opq->io.fromLwr.raw);
        TNBuffs_init(&opq->io.fromLwr.proc);
        TNBuffs_init(&opq->io.fromUp.raw);
        TNBuffs_init(&opq->io.fromUp.proc);
        //
        TNBuffs_create(&opq->io.fromLwr.raw, (TN_CORE_CONN_BUFF_SZ / (3 * 4)) * (3 * 4));   //must be multiple of 3 and 4 (base64 encode/decode sizes)
        TNBuffs_create(&opq->io.fromLwr.proc, (TN_CORE_CONN_BUFF_SZ / (3 * 4)) * (3 * 4));   //must be multiple of 3 and 4 (base64 encode/decode sizes)
        TNBuffs_create(&opq->io.fromUp.raw, (TN_CORE_CONN_BUFF_SZ / (3 * 4)) * (3 * 4)); //must be multiple of 3 and 4 (base64 encode/decode sizes)
        TNBuffs_create(&opq->io.fromUp.proc, (TN_CORE_CONN_BUFF_SZ / (3 * 4)) * (3 * 4)); //must be multiple of 3 and 4 (base64 encode/decode sizes)
    }
}

void TNLyrBase64_uninitLocked(STNBObject* obj){
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)obj;
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
        TNBuffs_release(&opq->io.fromLwr.raw);
        TNBuffs_release(&opq->io.fromLwr.proc);
        TNBuffs_release(&opq->io.fromUp.raw);
        TNBuffs_release(&opq->io.fromUp.proc);
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
BOOL TNLyrBase64_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData); //set next lyr iterface
//data flow
void TNLyrBase64_lyrStart_(void* usrData);        //start processing and upperLyr
BOOL TNLyrBase64_lyrIsRunning_(void* usrData);    //cleanup must wait
void TNLyrBase64_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrBase64_lyrShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrBase64_lyrClose_(void* usrData);
//dbg
void TNLyrBase64_lyrConcat_(STNBString* dst, void* usrData);

//

BOOL TNLyrBase64_getLyrItf(STTNLyrBase64Ref ref, STTNLyrLstnr* dst){
    BOOL r = FALSE;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)ref.opaque; NBASSERT(TNLyrBase64_isClass(ref))
    if(dst != NULL){
        NBObject_lock(opq);
        {
            NBMemory_setZeroSt(dst->itf, STTNLyrLstnrItf);
            dst->itf.lyrRetain     = NBObjRef_retainOpq;
            dst->itf.lyrRelease    = NBObjRef_releaseOpq;
            //lyr chain
            dst->itf.lyrSetNext    = TNLyrBase64_lyrSetNext_; //set next lyr iterface
            //data flow
            dst->itf.lyrStart       = TNLyrBase64_lyrStart_;
            dst->itf.lyrIsRunning   = TNLyrBase64_lyrIsRunning_; //cleanup must wait
            dst->itf.lyrConsumeMask = TNLyrBase64_lyrConsumeMask_;
            dst->itf.lyrShutdown    = TNLyrBase64_lyrShutdown_;
            dst->itf.lyrClose       = TNLyrBase64_lyrClose_;
            //dbg
            dst->itf.lyrConcat      = TNLyrBase64_lyrConcat_;
            //
            dst->usrParam           = opq;
            r = TRUE;
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrBase64_setParentStopFlag(STTNLyrBase64Ref ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)ref.opaque; NBASSERT(TNLyrBase64_isClass(ref))
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

BOOL TNLyrBase64_prepare(STTNLyrBase64Ref ref, const ENTNLyrFlow flow){
    BOOL r = FALSE;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)ref.opaque; NBASSERT(TNLyrBase64_isClass(ref))
    if(flow == ENTNLyrFlow_FromUp || flow == ENTNLyrFlow_FromLwr){
        NBObject_lock(opq);
        if(!opq->isPrepared){
            r = TRUE;
            //apply
            if(r){
                opq->flow = flow;
                opq->isPrepared = TRUE;
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

//lyr chain

BOOL TNLyrBase64_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData){ //set next lyr iterface
    BOOL r = FALSE;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrBase64_lyrStart_(void* usrData){
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
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

BOOL TNLyrBase64_lyrIsRunning_(void* usrData){    //cleanup must wait
    BOOL r = FALSE;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
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

void TNLyrBase64_consumeRawBuffsFromLwrLyrOpq_(STTNLyrBase64Opq* opq){
    STTNBuffs* buffsRaw = &opq->io.fromLwr.raw;
    STTNBuffs* buffsProc = &opq->io.fromLwr.proc;
    switch(opq->flow){
        case ENTNLyrFlow_FromLwr:
            //encode
            while(TNBuffs_canConsumeSz(buffsRaw, 3) && TNBuffs_canFillSz(buffsProc, 4)){
                NBBase64_code3Bytes(&buffsRaw->read->data[buffsRaw->read->csmd], 3, (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, 3);
                TNBuffs_moveFillCursor(buffsProc, 4);
                //PRINTF_INFO("TNLyrBase64, fromLwr encoded 4 bytes.\n");
            }
            //flush
            if(TNBuffs_canFillSz(buffsProc, 4) && TNLyrBase64_needFlushOutgoing(opq)){
                const UI32 dataSz = (buffsRaw->read->filled - buffsRaw->read->csmd); NBASSERT(dataSz > 0 && dataSz <= 3)
                NBBase64_code3Bytes(&buffsRaw->read->data[buffsRaw->read->csmd], dataSz, (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, dataSz);
                TNBuffs_moveFillCursor(buffsProc, 4);
                //PRINTF_INFO("TNLyrBase64, fromLwr encoded 4 bytes (flush).\n");
            }
            break;
        case ENTNLyrFlow_FromUp:
            //decode
            while(TNBuffs_canConsumeSz(buffsRaw, 4) && TNBuffs_canFillSz(buffsProc, 3)){
                const UI8 decSz = NBBase64_decode4Bytes((const char*)&buffsRaw->read->data[buffsRaw->read->csmd], (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, 4);
                TNBuffs_moveFillCursor(buffsProc, decSz);
                //PRINTF_INFO("TNLyrBase64, fromLwr decoded = %d bytes.\n", decSz);
                //base 64 decode error
                if(decSz == 0){
                    opq->lyr.fromMe.toLwr.read.isShuttedDown = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
                    PRINTF_ERROR("TNLyrBase64, fromLwr input is not base64.\n");
                    break;
                }
            }
            break;
        default:
            NBASSERT(FALSE); //unexpected flow
            break;
    }
}

void TNLyrBase64_consumeRawBuffsFromUpLyrOpq_(STTNLyrBase64Opq* opq){
    //consume-raw
    STTNBuffs* buffsRaw = &opq->io.fromUp.raw;
    STTNBuffs* buffsProc = &opq->io.fromUp.proc;
    switch(opq->flow){
        case ENTNLyrFlow_FromLwr:
            //decode
            while(TNBuffs_canConsumeSz(buffsRaw, 4) && TNBuffs_canFillSz(buffsProc, 3)){
                const UI8 decSz = NBBase64_decode4Bytes((const char*)&buffsRaw->read->data[buffsRaw->read->csmd], (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, 4);
                TNBuffs_moveFillCursor(buffsProc, decSz);
                //base 64 decode error
                if(decSz == 0){
                    opq->lyr.fromMe.toLwr.read.isShuttedDown = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
                    PRINTF_ERROR("TNLyrBase64, fromUp input is not base64.\n");
                    break;
                }
            }
            break;
        case ENTNLyrFlow_FromUp:
            //encode
            while(TNBuffs_canConsumeSz(buffsRaw, 3) && TNBuffs_canFillSz(buffsProc, 4) ){
                NBBase64_code3Bytes(&buffsRaw->read->data[buffsRaw->read->csmd], 3, (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, 3);
                TNBuffs_moveFillCursor(buffsProc, 4);
                //PRINTF_INFO("TNLyrBase64, fromUp encoded 4 bytes.\n");
            }
            //flush
            if(TNBuffs_canFillSz(buffsProc, 4) && TNLyrBase64_needFlushIncoming(opq)){
                const UI32 dataSz = (buffsRaw->read->filled - buffsRaw->read->csmd); NBASSERT(dataSz > 0 && dataSz <= 3)
                NBBase64_code3Bytes(&buffsRaw->read->data[buffsRaw->read->csmd], dataSz, (char*)&buffsProc->fill->data[buffsProc->fill->filled]);
                TNBuffs_moveCsmCursor(buffsRaw, dataSz);
                TNBuffs_moveFillCursor(buffsProc, 4);
                opq->lyr.toMe.fromUp.write.isFlushCalc = TRUE;
                //PRINTF_INFO("TNLyrBase64, fromUp encoded 4 bytes (flush).\n");
            }
            break;
        default:
            NBASSERT(FALSE); //unexpected flow
            break;
    }
}

void TNLyrBase64_readFromLwrLyrOpq_(STTNLyrBase64Opq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgReadsCount = 0;)
    STTNBuffs* buffsRaw = &opq->io.fromLwr.raw;
    while(!opq->lyr.fromMe.toLwr.read.isBlocked && !opq->lyr.fromMe.toLwr.read.isShuttedDown && TNBuffs_canFill(buffsRaw)){
        UI8* dst = &buffsRaw->fill->data[buffsRaw->fill->filled];
        const UI32 dstSz = (buffsRaw->fill->size - buffsRaw->fill->filled);
        const SI32 rr = (*lnk->itf.ioRead)(dst, dstSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrBase64, ioRead failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.read.isBlocked = opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                TNBuffs_moveFillCursor(buffsRaw, rr);
                //PRINTF_INFO("TNLyrBase64, (%s) ioRead from down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                //data populated, try to consume filled data
                TNLyrBase64_consumeRawBuffsFromLwrLyrOpq_(opq);
            }
            opq->lyr.fromMe.toLwr.read.isBlocked = (rr < dstSz);
        }
        IF_NBASSERT(dbgReadsCount++;)
        IF_NBASSERT(if(dbgReadsCount > 3){ PRINTF_WARNING("TNLyrBase64, %d reads.\n", dbgReadsCount); })
    }
    //flush data
    if(TNLyrBase64_needFlush(opq)){
        TNLyrBase64_consumeRawBuffsFromLwrLyrOpq_(opq);
    }
}

void TNLyrBase64_writeToLwrLyrOpq_(STTNLyrBase64Opq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgWritesCount = 0;)
    STTNBuffs* buffs = &opq->io.fromUp.proc;
    while(TNBuffs_canConsume(buffs) && !opq->lyr.fromMe.toLwr.write.isBlocked && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        const void* src = &buffs->read->data[buffs->read->csmd];
        const UI32 srcSz = (buffs->read->filled - buffs->read->csmd);
        const SI32 rr = (*lnk->itf.ioWrite)(src, srcSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrBase64, ioWrite failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.write.isBlocked = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                //PRINTF_INFO("TNLyrBase64, (%s) ioWrite to down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                //data should be already masked/unmasked
                TNBuffs_moveCsmCursor(buffs, rr);
                //data consumed, try to populate emptied space
                TNLyrBase64_consumeRawBuffsFromUpLyrOpq_(opq);
            }
            opq->lyr.fromMe.toLwr.write.isBlocked = (rr < srcSz);
        }
        IF_NBASSERT(dbgWritesCount++;)
        IF_NBASSERT(if(dbgWritesCount > 3){ PRINTF_WARNING("TNLyrBase64, %d writes.\n", dbgWritesCount); })
    }
    //flush
    if(!TNBuffs_canConsume(buffs) && opq->lyr.toMe.fromUp.write.isFlushPend && opq->lyr.toMe.fromUp.write.isFlushCalc && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        opq->lyr.toMe.fromUp.write.isFlushPend = opq->lyr.toMe.fromUp.write.isFlushCalc = FALSE;
        if(lnk->itf.ioFlush != NULL){
            (*lnk->itf.ioFlush)(lnk->usrData);
        }
    }
}

void TNLyrBase64_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData){
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
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
        TNLyrBase64_readFromLwrLyrOpq_(opq, lnk);
        //write to lower-lyr
        TNLyrBase64_writeToLwrLyrOpq_(opq, lnk);
        //push to nxt-lyr
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask != NULL){
            NBASSERT(!opq->lyr.toMe.lnkIsEnabled)
            opq->lyr.toMe.lnkIsEnabled = TRUE;
            {
                pollReqNxt = 0;
                const UI8 pollMaskNxt = (TNBuffs_canConsume(&opq->io.fromLwr.proc) ? ENNBIOPollsterOpBit_Read : 0) | (TNBuffs_canFill(&opq->io.fromUp.raw) ? ENNBIOPollsterOpBit_Write : 0);
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask)(&opq->lyr.toMe.lnk, pollMaskNxt, &pollReqNxt, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
            opq->lyr.toMe.lnkIsEnabled = FALSE;
        }
        //apply stopFlag (inmediatly)
        if(NBStopFlag_isAnyActivated(opq->stopFlag)){
            opq->lyr.fromMe.toLwr.read.isShuttedDown = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
        }
        //apply upper shutdowns to me (after consumeMask)
        {
            if(!opq->lyr.fromMe.toLwr.read.isShuttedDown && ((opq->lyr.toMe.fromLwr.read.isShuttedDown && (!TNBuffs_canConsume(&opq->io.fromLwr.proc) && !TNLyrBase64_needFlush(opq)) /*can flush*/) || opq->lyr.toMe.fromUp.read.isShuttedDown /*inmediate*/)){
                //shutdown my read
                //PRINTF_INFO("TNLyrBase64, shutting read after %s layer's.\n", opq->lyr.toMe.fromUp.read.isShuttedDown ? "upper" : "lwr");
                opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            }
            //
            if(!opq->lyr.fromMe.toLwr.write.isShuttedDown && (opq->lyr.toMe.fromLwr.write.isShuttedDown /*inmediate*/ || (opq->lyr.toMe.fromUp.write.isShuttedDown && (!TNBuffs_canConsume(&opq->io.fromUp.proc) && !TNLyrBase64_needFlush(opq)) /*can flush*/))){
                //shutdown my write
                //PRINTF_INFO("TNLyrBase64, shutting write after %s layer's.\n", opq->lyr.toMe.fromLwr.write.isShuttedDown ? "lwr" : "upper");
                opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
            }
        }
        //notify my shutdowns to upLyr and lwrLyr (after consumeMask)
        {
            UI8 shutMaskUp = 0, shutMaskLwr = 0;
            if(!opq->lyr.fromMe.toLwr.read.isShutNotified && opq->lyr.fromMe.toLwr.read.isShuttedDown){
                opq->lyr.fromMe.toLwr.read.isShutNotified = TRUE;
                if(!opq->lyr.toMe.fromLwr.read.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskLwr |= NB_IO_BIT_READ;
                }
                if(!opq->lyr.toMe.fromUp.read.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_READ;
                }
            }
            if(!opq->lyr.fromMe.toLwr.write.isShutNotified && opq->lyr.fromMe.toLwr.write.isShuttedDown){
                opq->lyr.fromMe.toLwr.write.isShutNotified = TRUE;
                if(!opq->lyr.toMe.fromLwr.write.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskLwr |= NB_IO_BIT_WRITE;
                }
                if(!opq->lyr.toMe.fromUp.write.isShuttedDown){ //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_WRITE;
                }
            }
            IF_PRINTF(
                      if((shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL) || shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL){
                          //PRINTF_INFO("TNLyrBase64, notifying %s%s%s%s %s%s%s%s.\n", (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? "lwrLyr-shut(" : ""), (shutMaskLwr & NB_IO_BIT_READ ? "rd" : ""), (shutMaskLwr & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? ")" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? "upLyr-shut(" : ""), (shutMaskUp & NB_IO_BIT_READ ? "rd" : ""), (shutMaskUp & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? ")" : ""));
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
            const BOOL imBussy = (!opq->lyr.fromMe.toLwr.read.isShuttedDown || !opq->lyr.fromMe.toLwr.read.isShutNotified || !opq->lyr.fromMe.toLwr.write.isShuttedDown || !opq->lyr.fromMe.toLwr.write.isShutNotified);
            if(!imBussy){
                const int v0 = (opq->io.fromLwr.raw.read->filled - opq->io.fromLwr.raw.read->csmd);
                const int v1 = (opq->io.fromLwr.proc.read->filled - opq->io.fromLwr.proc.read->csmd);
                const int v2 = (opq->io.fromUp.raw.read->filled - opq->io.fromUp.raw.read->csmd);
                const int v3 = (opq->io.fromUp.proc.read->filled - opq->io.fromUp.proc.read->csmd);
                if(v0 != 0 || v1 != 0 || v2 != 0 || v3 != 0){
                    PRINTF_INFO("TNLyrBase64, not-bussy lwr-raw(%d bytes)-prc(%d bytes) upper-raw(%d bytes)-prc(%d bytes).\n", v0, v1, v2, v3);
                }
            }
        }
    }
    NBObject_unlock(opq);
}

void TNLyrBase64_lyrShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrBase64_lyrClose_(void* usrData){
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        opq->lyr.toMe.fromLwr.read.isShuttedDown = opq->lyr.toMe.fromLwr.write.isShuttedDown = TRUE;
    }
    NBObject_unlock(opq);
}

void TNLyrBase64_lyrConcat_(STNBString* dst, void* usrData) {
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    if (dst != NULL) {
        NBObject_lock(opq);
        {
            NBString_concat(dst, "TNLyrB64 (");
            NBString_concat(dst, (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
            NBString_concat(dst, ") r("); NBString_concat(dst, opq->lyr.fromMe.toLwr.read.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ") w("); NBString_concat(dst, opq->lyr.fromMe.toLwr.write.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ")\n");
            if (opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask == NULL) {
                NBString_concat(dst, "end-of-lyrs\n");
            } else if(opq->lyr.fromMe.toUp.lstnr.itf.lyrConcat == NULL){
                NBString_concat(dst, "upper-lyrs-missing-lyrConcat\n");
            } else {
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrConcat)(dst, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
        }
        NBObject_unlock(opq);
    }
}

//io

SI32 TNLyrBase64_ioRead_(void* pDst, const SI32 dstSz, void* usrData) { //read data to destination buffer, returns the ammount of bytes read, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.read.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be reading anymore.
        } else if(dstSz == 0){
            r = 0;
        } else if(dstSz > 0){
            STTNBuffs* buffs = &opq->io.fromLwr.proc;
            //Data should be already masked/unmasked
            r = TNBuffs_consume(buffs, pDst, dstSz);
            if(r > 0){
                //data consumed, try to populate emptied space
                TNLyrBase64_consumeRawBuffsFromLwrLyrOpq_(opq);
            } else if(r == 0 && opq->lyr.fromMe.toLwr.read.isShuttedDown){
                //ERROR or EOF
                PRINTF_INFO("TNLyrBase64, ioRead sending ERROR-EOF-sginal.\n");
                r = NB_IO_ERROR;
            }
        }
    }
    return r;
}

SI32 TNLyrBase64_ioWrite_(const void* pSrc, const SI32 srcSz, void* usrData){ //write data from source buffer, returns the ammount of bytes written, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.write.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be writting anymore.
        } else if(srcSz == 0){
            r = 0;
        } else if(srcSz > 0){
            if(opq->lyr.toMe.fromUp.write.isFlushPend){
                r = 0;
            } else {
                STTNBuffs* buffsRaw = &opq->io.fromUp.raw;
                const BYTE* src = (const BYTE*)pSrc;
                r = 0;
                while(r < srcSz && buffsRaw->fill->filled < buffsRaw->fill->size){
                    UI8* dst                = (UI8*)&buffsRaw->fill->data[buffsRaw->fill->filled];
                    const SI32 srcAvail     = (srcSz - r);
                    const SI32 fillAvail    = (buffsRaw->fill->size - buffsRaw->fill->filled);
                    const SI32 moveSz       = (srcAvail < fillAvail ? srcAvail : fillAvail);
                    NBASSERT(moveSz > 0)
                    NBMemory_copy(dst, &src[r], moveSz);
                    r += moveSz;
                    //
                    TNBuffs_moveFillCursor(buffsRaw, moveSz);
                    //data populated, try to consume filled data
                    TNLyrBase64_consumeRawBuffsFromUpLyrOpq_(opq);
                }
            }
        }
    }
    return r;
}

void TNLyrBase64_ioFlush_(void* usrData){ //flush write-data
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.write.isFlushPend = TRUE;
    }
}

void TNLyrBase64_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrBase64_ioClose_(void* usrData){ //close ungracefully
    STTNLyrBase64Opq* opq = (STTNLyrBase64Opq*)usrData; NBASSERT(TNLyrBase64_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.read.isShuttedDown = opq->lyr.toMe.fromUp.write.isShuttedDown = TRUE;
    }
}
