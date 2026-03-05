//
//  TNLyrDump.c
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
#include "core/TNLyrDump.h"

//TNLyrDump

typedef struct STTNLyrDumpOpq_ {
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
    //file
    struct {
        STNBFileRef toUpr;
        STNBFileRef toLwr;
    } file;
} STTNLyrDumpOpq;

//

NB_OBJREF_BODY(TNLyrDump, STTNLyrDumpOpq, NBObject)

#define TNLyrDump_needFlushIncoming(OPQ)      (TNBuffs_canConsume(&opq->io.fromUp.raw) && ((OPQ)->lyr.toMe.fromUp.write.isShuttedDown || ((OPQ)->lyr.toMe.fromUp.write.isFlushPend && !(OPQ)->lyr.toMe.fromUp.write.isFlushCalc)))
#define TNLyrDump_needFlushOutgoing(OPQ)      (TNBuffs_canConsume(&opq->io.fromLwr.raw) && (((OPQ)->lyr.toMe.fromLwr.read.isShuttedDown || (OPQ)->lyr.fromMe.toLwr.read.isShuttedDown) && !(OPQ)->lyr.toMe.fromUp.read.isShuttedDown))
#define TNLyrDump_needFlush(OPQ)              ((OPQ)->flow == ENTNLyrFlow_FromUp && TNLyrDump_needFlushIncoming(OPQ) || (OPQ)->flow == ENTNLyrFlow_FromLwr && TNLyrDump_needFlushOutgoing(OPQ))

//

SI32 TNLyrDump_ioRead_(void* dst, const SI32 dstSz, void* usrData); //read data to destination buffer, returns the ammount of bytes read, negative in case of error
SI32 TNLyrDump_ioWrite_(const void* src, const SI32 srcSz, void* usrData); //write data from source buffer, returns the ammount of bytes written, negative in case of error
void TNLyrDump_ioFlush_(void* usrData);       //flush write-data
void TNLyrDump_ioShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrDump_ioClose_(void* usrData);       //close ungracefully

//

void TNLyrDump_initZeroed(STNBObject* obj){
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)obj;
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
            opq->lyr.toMe.lnk.itf.ioRead     = TNLyrDump_ioRead_;
            opq->lyr.toMe.lnk.itf.ioWrite    = TNLyrDump_ioWrite_;
            opq->lyr.toMe.lnk.itf.ioFlush    = TNLyrDump_ioFlush_;
            opq->lyr.toMe.lnk.itf.ioShutdown = TNLyrDump_ioShutdown_;
            opq->lyr.toMe.lnk.itf.ioClose    = TNLyrDump_ioClose_;
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

void TNLyrDump_uninitLocked(STNBObject* obj){
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)obj;
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
    //file
    {
        //toUp
        {
            if(NBFile_isSet(opq->file.toUpr)){
                NBFile_close(opq->file.toUpr);
            }
            NBFile_release(&opq->file.toUpr);
            NBFile_null(&opq->file.toUpr);
        }
        //toLwr
        {
            if(NBFile_isSet(opq->file.toLwr)){
                NBFile_close(opq->file.toLwr);
            }
            NBFile_release(&opq->file.toLwr);
            NBFile_null(&opq->file.toLwr);
        }
    }
    NBThreadCond_release(&opq->cond);
}

//STTNLyrLstnr
//lyr chain
BOOL TNLyrDump_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData); //set next lyr iterface
//data flow
void TNLyrDump_lyrStart_(void* usrData);        //start processing and upperLyr
BOOL TNLyrDump_lyrIsRunning_(void* usrData);    //cleanup must wait
void TNLyrDump_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrDump_lyrShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrDump_lyrClose_(void* usrData);
//dbg
void TNLyrDump_lyrConcat_(STNBString* dst, void* usrData);

//

BOOL TNLyrDump_getLyrItf(STTNLyrDumpRef ref, STTNLyrLstnr* dst){
    BOOL r = FALSE;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)ref.opaque; NBASSERT(TNLyrDump_isClass(ref))
    if(dst != NULL){
        NBObject_lock(opq);
        {
            NBMemory_setZeroSt(dst->itf, STTNLyrLstnrItf);
            dst->itf.lyrRetain     = NBObjRef_retainOpq;
            dst->itf.lyrRelease    = NBObjRef_releaseOpq;
            //lyr chain
            dst->itf.lyrSetNext    = TNLyrDump_lyrSetNext_; //set next lyr iterface
            //data flow
            dst->itf.lyrStart       = TNLyrDump_lyrStart_;
            dst->itf.lyrIsRunning   = TNLyrDump_lyrIsRunning_; //cleanup must wait
            dst->itf.lyrConsumeMask = TNLyrDump_lyrConsumeMask_;
            dst->itf.lyrShutdown    = TNLyrDump_lyrShutdown_;
            dst->itf.lyrClose       = TNLyrDump_lyrClose_;
            //dbg
            dst->itf.lyrConcat      = TNLyrDump_lyrConcat_;
            //
            dst->usrParam           = opq;
            r = TRUE;
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrDump_setParentStopFlag(STTNLyrDumpRef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)ref.opaque; NBASSERT(TNLyrDump_isClass(ref))
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

BOOL TNLyrDump_prepare(STTNLyrDumpRef ref, const ENTNLyrFlow flow, const char* pathPrefix){
    BOOL r = FALSE;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)ref.opaque; NBASSERT(TNLyrDump_isClass(ref))
    if(flow == ENTNLyrFlow_FromUp || flow == ENTNLyrFlow_FromLwr){
        NBObject_lock(opq);
        if(!opq->isPrepared){
            r = TRUE;
            //close current files
            {
                //toUp
                {
                    if(NBFile_isSet(opq->file.toUpr)){
                        NBFile_close(opq->file.toUpr);
                    }
                    NBFile_release(&opq->file.toUpr);
                    NBFile_null(&opq->file.toUpr);
                }
                //toLwr
                {
                    if(NBFile_isSet(opq->file.toLwr)){
                        NBFile_close(opq->file.toLwr);
                    }
                    NBFile_release(&opq->file.toLwr);
                    NBFile_null(&opq->file.toLwr);
                }
            }
            //open file
            {
                //toUpr
                UI32 fileSeq = 0, fileSeqMax = 1024;
                while(fileSeq < fileSeqMax) {
                    STNBFileRef file = STNBObjRef_Zero;
                    const STNBDatetime now = NBDatetime_getCurLocal();
                    STNBString path;
                    NBString_initWithStr(&path, pathPrefix);
                    NBString_concatByte(&path, '_');
                    NBString_concatDateTimeCompact(&path, now);
                    NBString_concatByte(&path, '_');
                    NBString_concatUI64(&path, *((UI64*)opq));
                    if(fileSeq > 0){
                        NBString_concatByte(&path, '_');
                        NBString_concatUI32(&path, fileSeq);
                    }
                    NBString_concat(&path, "_up.dump");
                    //create file
                    file = NBFile_alloc(NULL);
                    if(NBFile_open(file, path.str, ENNBFileMode_Write)){
                        //set current
                        opq->file.toUpr = file;
                        break;
                    }
                    NBString_release(&path);
                    NBFile_release(&file);
                    ++fileSeq;
                }
                //validate if file was created
                if(!NBFile_isSet(opq->file.toUpr)){
                    r = false;
                } else {
                    fileSeqMax = fileSeq + 1024;
                    while(fileSeq < fileSeqMax) {
                        STNBFileRef file = STNBObjRef_Zero;
                        const STNBDatetime now = NBDatetime_getCurLocal();
                        STNBString path;
                        NBString_initWithStr(&path, pathPrefix);
                        NBString_concatByte(&path, '_');
                        NBString_concatDateTimeCompact(&path, now);
                        NBString_concatByte(&path, '_');
                        NBString_concatUI64(&path, *((UI64*)opq));
                        if(fileSeq > 0){
                            NBString_concatByte(&path, '_');
                            NBString_concatUI32(&path, fileSeq);
                        }
                        NBString_concat(&path, "_dwn.dump");
                        //create file
                        file = NBFile_alloc(NULL);
                        if(NBFile_open(file, path.str, ENNBFileMode_Write)){
                            //set current
                            opq->file.toLwr = file;
                            break;
                        }
                        NBString_release(&path);
                        NBFile_release(&file);
                        ++fileSeq;
                    }
                }
                if(!NBFile_isSet(opq->file.toUpr) || !NBFile_isSet(opq->file.toLwr)){
                    r = false;
                }
            }
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

BOOL TNLyrDump_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData){ //set next lyr iterface
    BOOL r = FALSE;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrDump_lyrStart_(void* usrData){
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

BOOL TNLyrDump_lyrIsRunning_(void* usrData){    //cleanup must wait
    BOOL r = FALSE;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrDump_consumeRawBuffsFromLwrLyrOpq_(STTNLyrDumpOpq* opq){
    STTNBuffs* buffsRaw = &opq->io.fromLwr.raw;
    STTNBuffs* buffsProc = &opq->io.fromLwr.proc;
    //
    UI64 csmAvail = TNBuffs_csmAvailSz(buffsRaw);
    UI64 fillAvail = TNBuffs_fillAvailSz(buffsProc);
    while(csmAvail > 0 && fillAvail > 0){
        const SI32 smallerSz = (SI32)(csmAvail < fillAvail ? csmAvail : fillAvail);
        const BYTE* src = &buffsRaw->read->data[buffsRaw->read->csmd];
        BYTE* dst = &buffsProc->fill->data[buffsProc->fill->filled];
        NBMemory_copy(dst, src, smallerSz);
        //dump
        STNBFileRef* file = &opq->file.toUpr;
        if(NBFile_isSet(*file)){
            NBFile_lock(*file);
            const SI32 written = NBFile_write(*file, src, smallerSz);
            NBFile_unlock(*file);
            if(written != smallerSz){
                NBFile_close(*file);
                NBFile_release(file);
                NBFile_null(file);
            }
        }
        //move cursors
        TNBuffs_moveCsmCursor(buffsRaw, smallerSz);
        TNBuffs_moveFillCursor(buffsProc, smallerSz);
        //PRINTF_INFO("TNLyrDump, fromLwr encoded 4 bytes.\n");
        //check again (in case internal buffers changed)
        csmAvail = TNBuffs_csmAvailSz(buffsRaw);
        fillAvail = TNBuffs_fillAvailSz(buffsProc);
    }
}

void TNLyrDump_consumeRawBuffsFromUpLyrOpq_(STTNLyrDumpOpq* opq){
    //consume-raw
    STTNBuffs* buffsRaw = &opq->io.fromUp.raw;
    STTNBuffs* buffsProc = &opq->io.fromUp.proc;
    //
    UI64 csmAvail = TNBuffs_csmAvailSz(buffsRaw);
    UI64 fillAvail = TNBuffs_fillAvailSz(buffsProc);
    while(csmAvail > 0 && fillAvail > 0){
        const SI32 smallerSz = (SI32)(csmAvail < fillAvail ? csmAvail : fillAvail);
        const BYTE* src = &buffsRaw->read->data[buffsRaw->read->csmd];
        BYTE* dst = &buffsProc->fill->data[buffsProc->fill->filled];
        NBMemory_copy(dst, src, smallerSz);
        //dump
        STNBFileRef* file = &opq->file.toLwr;
        if(NBFile_isSet(*file)){
            NBFile_lock(*file);
            const SI32 written = NBFile_write(*file, src, smallerSz);
            NBFile_unlock(*file);
            if(written != smallerSz){
                NBFile_close(*file);
                NBFile_release(file);
                NBFile_null(file);
            }
        }
        //move cursors
        TNBuffs_moveCsmCursor(buffsRaw, smallerSz);
        TNBuffs_moveFillCursor(buffsProc, smallerSz);
        //PRINTF_INFO("TNLyrDump, fromLwr encoded 4 bytes.\n");
        //check again (in case internal buffers changed)
        csmAvail = TNBuffs_csmAvailSz(buffsRaw);
        fillAvail = TNBuffs_fillAvailSz(buffsProc);
    }
}

void TNLyrDump_readFromLwrLyrOpq_(STTNLyrDumpOpq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgReadsCount = 0;)
    STTNBuffs* buffsRaw = &opq->io.fromLwr.raw;
    while(!opq->lyr.fromMe.toLwr.read.isBlocked && !opq->lyr.fromMe.toLwr.read.isShuttedDown && TNBuffs_canFill(buffsRaw)){
        UI8* dst = &buffsRaw->fill->data[buffsRaw->fill->filled];
        const UI32 dstSz = (buffsRaw->fill->size - buffsRaw->fill->filled);
        const SI32 rr = (*lnk->itf.ioRead)(dst, dstSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrDump, ioRead failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.read.isBlocked = opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                TNBuffs_moveFillCursor(buffsRaw, rr);
                //PRINTF_INFO("TNLyrDump, (%s) ioRead from down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                //data populated, try to consume filled data
                TNLyrDump_consumeRawBuffsFromLwrLyrOpq_(opq);
            }
            opq->lyr.fromMe.toLwr.read.isBlocked = (rr < dstSz);
        }
        IF_NBASSERT(dbgReadsCount++;)
        IF_NBASSERT(if(dbgReadsCount > 3){ PRINTF_WARNING("TNLyrDump, %d reads.\n", dbgReadsCount); })
    }
    //flush data
    if(TNLyrDump_needFlush(opq)){
        TNLyrDump_consumeRawBuffsFromLwrLyrOpq_(opq);
    }
}

void TNLyrDump_writeToLwrLyrOpq_(STTNLyrDumpOpq* opq, const STNBIOLnk* lnk){
    IF_NBASSERT(UI32 dbgWritesCount = 0;)
    STTNBuffs* buffs = &opq->io.fromUp.proc;
    while(TNBuffs_canConsume(buffs) && !opq->lyr.fromMe.toLwr.write.isBlocked && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        const void* src = &buffs->read->data[buffs->read->csmd];
        const UI32 srcSz = (buffs->read->filled - buffs->read->csmd);
        const SI32 rr = (*lnk->itf.ioWrite)(src, srcSz, lnk->usrData);
        if(rr < 0){
            PRINTF_ERROR("TNLyrDump, ioWrite failed down-lyr (%d).\n", rr);
            opq->lyr.fromMe.toLwr.write.isBlocked = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
            break;
        } else {
            if(rr > 0){
                //PRINTF_INFO("TNLyrDump, (%s) ioWrite to down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                //data should be already masked/unmasked
                TNBuffs_moveCsmCursor(buffs, rr);
                //data consumed, try to populate emptied space
                TNLyrDump_consumeRawBuffsFromUpLyrOpq_(opq);
            }
            opq->lyr.fromMe.toLwr.write.isBlocked = (rr < srcSz);
        }
        IF_NBASSERT(dbgWritesCount++;)
        IF_NBASSERT(if(dbgWritesCount > 3){ PRINTF_WARNING("TNLyrDump, %d writes.\n", dbgWritesCount); })
    }
    //flush
    if(!TNBuffs_canConsume(buffs) && opq->lyr.toMe.fromUp.write.isFlushPend && opq->lyr.toMe.fromUp.write.isFlushCalc && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
        opq->lyr.toMe.fromUp.write.isFlushPend = opq->lyr.toMe.fromUp.write.isFlushCalc = FALSE;
        if(lnk->itf.ioFlush != NULL){
            (*lnk->itf.ioFlush)(lnk->usrData);
        }
    }
}

void TNLyrDump_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData){
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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
        TNLyrDump_readFromLwrLyrOpq_(opq, lnk);
        //write to lower-lyr
        TNLyrDump_writeToLwrLyrOpq_(opq, lnk);
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
            if(!opq->lyr.fromMe.toLwr.read.isShuttedDown && ((opq->lyr.toMe.fromLwr.read.isShuttedDown && (!TNBuffs_canConsume(&opq->io.fromLwr.proc) && !TNLyrDump_needFlush(opq)) /*can flush*/) || opq->lyr.toMe.fromUp.read.isShuttedDown /*inmediate*/)){
                //shutdown my read
                //PRINTF_INFO("TNLyrDump, shutting read after %s layer's.\n", opq->lyr.toMe.fromUp.read.isShuttedDown ? "upper" : "lwr");
                opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
            }
            //
            if(!opq->lyr.fromMe.toLwr.write.isShuttedDown && (opq->lyr.toMe.fromLwr.write.isShuttedDown /*inmediate*/ || (opq->lyr.toMe.fromUp.write.isShuttedDown && (!TNBuffs_canConsume(&opq->io.fromUp.proc) && !TNLyrDump_needFlush(opq)) /*can flush*/))){
                //shutdown my write
                //PRINTF_INFO("TNLyrDump, shutting write after %s layer's.\n", opq->lyr.toMe.fromLwr.write.isShuttedDown ? "lwr" : "upper");
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
                          //PRINTF_INFO("TNLyrDump, notifying %s%s%s%s %s%s%s%s.\n", (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? "lwrLyr-shut(" : ""), (shutMaskLwr & NB_IO_BIT_READ ? "rd" : ""), (shutMaskLwr & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? ")" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? "upLyr-shut(" : ""), (shutMaskUp & NB_IO_BIT_READ ? "rd" : ""), (shutMaskUp & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? ")" : ""));
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
                    PRINTF_INFO("TNLyrDump, not-bussy lwr-raw(%d bytes)-prc(%d bytes) upper-raw(%d bytes)-prc(%d bytes).\n", v0, v1, v2, v3);
                }
            }
        }
    }
    NBObject_unlock(opq);
}

void TNLyrDump_lyrShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrDump_lyrClose_(void* usrData){
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        opq->lyr.toMe.fromLwr.read.isShuttedDown = opq->lyr.toMe.fromLwr.write.isShuttedDown = TRUE;
    }
    NBObject_unlock(opq);
}

void TNLyrDump_lyrConcat_(STNBString* dst, void* usrData) {
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

SI32 TNLyrDump_ioRead_(void* pDst, const SI32 dstSz, void* usrData) { //read data to destination buffer, returns the ammount of bytes read, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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
                TNLyrDump_consumeRawBuffsFromLwrLyrOpq_(opq);
            } else if(r == 0 && opq->lyr.fromMe.toLwr.read.isShuttedDown){
                //ERROR or EOF
                PRINTF_INFO("TNLyrDump, ioRead sending ERROR-EOF-sginal.\n");
                r = NB_IO_ERROR;
            }
        }
    }
    return r;
}

SI32 TNLyrDump_ioWrite_(const void* pSrc, const SI32 srcSz, void* usrData){ //write data from source buffer, returns the ammount of bytes written, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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
                    TNLyrDump_consumeRawBuffsFromUpLyrOpq_(opq);
                }
            }
        }
    }
    return r;
}

void TNLyrDump_ioFlush_(void* usrData){ //flush write-data
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.write.isFlushPend = TRUE;
    }
}

void TNLyrDump_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrDump_ioClose_(void* usrData){ //close ungracefully
    STTNLyrDumpOpq* opq = (STTNLyrDumpOpq*)usrData; NBASSERT(TNLyrDump_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.read.isShuttedDown = opq->lyr.toMe.fromUp.write.isShuttedDown = TRUE;
    }
}
