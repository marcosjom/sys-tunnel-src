//
//  TNLyrSsl.c
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
#include "core/TNBuffs.h"
#include "core/TNLyrSsl.h"

//TNLyrSslCmdState

typedef enum ENTNLyrSslCmdState_ {
    ENTNLyrSslCmdState_Unknown = 0,     //not triggered yet
    ENTNLyrSslCmdState_ReadHungry,      //read-hungry
    ENTNLyrSslCmdState_WriteHungry,     //write-hungry
    ENTNLyrSslCmdState_Completed,       //done
    //count
    ENTNLyrSslCmdState_Count
} ENTNLyrSslCmdState;

//TNLyrSsl

typedef struct STTNLyrSslOpq_ {
    STNBObject              prnt;
    STNBThreadCond          cond;
    STNBStopFlagRef         stopFlag;
    ENTNLyrFlow             flow;
    //lyr
    struct {
        //prv
        struct {
            UI8             pollMask;       //current pollMask
            //buffs
            struct {
                STTNBuffs   read;   //used only if outgoing flow
                STTNBuffs   write;  //used only if outgoing flow
            } buffs;
        } prv;
        //nxt
        struct {
            STTNLyrLstnr    lstnr;
            //buffs
            struct {
                STTNBuffs   read;   //used only if outgoing flow
                STTNBuffs   write;  //used only if outgoing flow
            } buffs;
        } nxt;
        //io
        struct {
            //toMe (sent to upper layer)
            struct {
                BOOL        lnkIsEnabled;   //link is only allowed to be called during 'nxt.consumeMask' call
                STNBIOLnk   lnk;            //link to expose myself sent to next layer
            } toMe;
            //toPrev (used by callbacks methods inside 'consumeMask' method)
            struct {
                BOOL        isEnabled;      //link is only allowed to be called during 'nxt.consumeMask' call
                STNBIOLnk   lnk;            //link to expose myself sent to next layer
            } toPrev;
        } io;
    } lyr;
    //ssl
    struct {
        STNBSslRef      conn;
        STNBSslContextRef ctx;
        STNBArray       CAs;    //STNBX509
        //handshake
        struct {
            ENTNLyrSslCmdState state;
        } handshake;
        //cmd
        struct {
            ENTNLyrSslCmdState state;
        } cmd;
        //cert
        struct {
            BOOL        isSignedByCA;
            STNBX509    cur;
        } cert;
    } ssl;
} STTNLyrSslOpq;

//

NB_OBJREF_BODY(TNLyrSsl, STTNLyrSslOpq, NBObject)

//

SI32 TNLyrSsl_ioRead_fromAcceptedConn_(void* dst, const SI32 dstSz, void* usrData);      //incoming conn
SI32 TNLyrSsl_ioWrite_toAcceptedConn_(const void* src, const SI32 srcSz, void* usrData); //incoming conn
//
SI32 TNLyrSsl_ioRead_fromConnectConn_(void* pDst, const SI32 dstSz, void* usrData);      //outgoing conn
SI32 TNLyrSsl_ioWrite_toConnectConn_(const void* pSrc, const SI32 srcSz, void* usrData); //outgoing conn
//
void TNLyrSsl_ioFlush_(void* usrData); //flush write-data
void TNLyrSsl_ioShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrSsl_ioClose_(void* usrData); //close ungracefully

//NBIOLink

SI32 TNLyrSsl_ssl_ioRead_fromPrev_(void* dst, const SI32 dstSz, void* usrData);         //incoming conn
SI32 TNLyrSsl_ssl_ioWrite_toPrev_(const void* src, const SI32 srcSz, void* usrData);    //incoming conn
//
SI32 TNLyrSsl_ssl_ioRead_fromNext_(void* dst, const SI32 dstSz, void* usrData);         //outgoing conn
SI32 TNLyrSsl_ssl_ioWrite_toNext_(const void* src, const SI32 srcSz, void* usrData);    //outgoing conn
//
void TNLyrSsl_ssl_ioFlush_(void* usrData); //flush write-data
void TNLyrSsl_ssl_ioShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrSsl_ssl_ioClose_(void* usrData); //close ungracefully

//

void TNLyrSsl_initZeroed(STNBObject* obj){
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)obj;
    NBThreadCond_init(&opq->cond);
    opq->stopFlag   = NBStopFlag_alloc(NULL);
    //cfg
    {
        //
    }
    //lyr
    {
        //io
        {
            opq->lyr.io.toMe.lnk.usrData        = opq;
            opq->lyr.io.toMe.lnk.itf.ioRead     = NULL; //to determine
            opq->lyr.io.toMe.lnk.itf.ioWrite    = NULL; //to detemine
            opq->lyr.io.toMe.lnk.itf.ioFlush    = TNLyrSsl_ioFlush_;
            opq->lyr.io.toMe.lnk.itf.ioShutdown = TNLyrSsl_ioShutdown_;
            opq->lyr.io.toMe.lnk.itf.ioClose    = TNLyrSsl_ioClose_;
        }
        //buffs
        {
            //prv
            TNBuffs_init(&opq->lyr.prv.buffs.read); //used only if outgoing flow
            TNBuffs_init(&opq->lyr.prv.buffs.write); //used only if outgoing flow
            //nxt
            TNBuffs_init(&opq->lyr.nxt.buffs.read); //used only if outgoing flow
            TNBuffs_init(&opq->lyr.nxt.buffs.write); //used only if outgoing flow
        }
    }
    //ssl
    {
        //CAs
        NBArray_init(&opq->ssl.CAs, sizeof(STNBX509), NULL);
        //cert
        {
            NBX509_init(&opq->ssl.cert.cur);
        }
    }
}

void TNLyrSsl_uninitLocked(STNBObject* obj){
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)obj;
    //activate stop-flag
    NBStopFlag_activate(opq->stopFlag);
    //lyr
    {
        if(opq->lyr.nxt.lstnr.itf.lyrRelease != NULL){
            (*opq->lyr.nxt.lstnr.itf.lyrRelease)(opq->lyr.nxt.lstnr.usrParam);
        }
        NBMemory_setZeroSt(opq->lyr.nxt.lstnr, STTNLyrLstnr);
        //buffs
        {
            //prv
            TNBuffs_release(&opq->lyr.prv.buffs.read); //used only if outgoing flow
            TNBuffs_release(&opq->lyr.prv.buffs.write); //used only if outgoing flow
            //nxt
            TNBuffs_release(&opq->lyr.nxt.buffs.read); //used only if outgoing flow
            TNBuffs_release(&opq->lyr.nxt.buffs.write); //used only if outgoing flow
        }
    }
    //ssl
    {
        if(NBSsl_isSet(opq->ssl.conn)){
            NBSsl_release(&opq->ssl.conn);
            NBSsl_null(&opq->ssl.conn);
        }
        if(NBSslContext_isSet(opq->ssl.ctx)){
            NBSslContext_release(&opq->ssl.ctx);
            NBSslContext_null(&opq->ssl.ctx);
        }
        //CAs
        {
            SI32 i; for(i = 0; i < opq->ssl.CAs.use; i++){
                STNBX509* ca = NBArray_itmPtrAtIndex(&opq->ssl.CAs, STNBX509, i);
                NBX509_release(ca);
            }
            NBArray_empty(&opq->ssl.CAs);
            NBArray_release(&opq->ssl.CAs);
        }
        //cert
        {
            opq->ssl.cert.isSignedByCA = FALSE;
            NBX509_release(&opq->ssl.cert.cur);
        }
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
BOOL TNLyrSsl_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData); //set next lyr iterface
//data flow
void TNLyrSsl_lyrStart_(void* usrData);
BOOL TNLyrSsl_lyrIsRunning_(void* usrData);
void TNLyrSsl_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrSsl_lyrShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrSsl_lyrClose_(void* usrData);
//dbg
void TNLyrSsl_lyrConcat_(STNBString* dst, void* usrData);

//

BOOL TNLyrSsl_getLyrItf(STTNLyrSslRef ref, STTNLyrLstnr* dst){
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)ref.opaque; NBASSERT(TNLyrSsl_isClass(ref))
    if(dst != NULL){
        NBObject_lock(opq);
        {
            NBMemory_setZeroSt(dst->itf, STTNLyrLstnrItf);
            dst->itf.lyrRetain     = NBObjRef_retainOpq;
            dst->itf.lyrRelease    = NBObjRef_releaseOpq;
            //lyr chain
            dst->itf.lyrSetNext    = TNLyrSsl_lyrSetNext_; //set next lyr iterface
            //data flow
            dst->itf.lyrStart       = TNLyrSsl_lyrStart_;
            dst->itf.lyrIsRunning   = TNLyrSsl_lyrIsRunning_;
            dst->itf.lyrConsumeMask = TNLyrSsl_lyrConsumeMask_;
            dst->itf.lyrShutdown    = TNLyrSsl_lyrShutdown_;
            dst->itf.lyrClose       = TNLyrSsl_lyrClose_;
            //dbg
            dst->itf.lyrConcat      = TNLyrSsl_lyrConcat_;
            //
            dst->usrParam           = opq;
            r = TRUE;
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrSsl_setParentStopFlag(STTNLyrSslRef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)ref.opaque; NBASSERT(TNLyrSsl_isClass(ref))
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

BOOL TNLyrSsl_startAcceptingConn(STTNLyrSslRef ref, STNBSslContextRef ctx, STNBX509* CAs, const UI32 CAsSz){
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)ref.opaque; NBASSERT(TNLyrSsl_isClass(ref))
    if(NBSslContext_isSet(ctx) && NBSslContext_isCreated(ctx)){
        NBObject_lock(opq);
        r = TRUE;
        NBASSERT(opq->ssl.handshake.state == ENTNLyrSslCmdState_Unknown)
        //ssl
        if(r){
            STNBSslRef ssl = NBSsl_alloc(NULL);
            STNBIOLnkItf itf;
            NBMemory_setZeroSt(itf, STNBIOLnkItf);
            //no-lock required
            itf.ioRetain   = NULL; //NBObjRef_retainOpq;
            itf.ioRelease  = NULL; //NBObjRef_releaseOpq;
            //
            itf.ioIsObjRef = NULL;
            //
            itf.ioRead     = TNLyrSsl_ssl_ioRead_fromPrev_; //incoming conn
            itf.ioWrite    = TNLyrSsl_ssl_ioWrite_toPrev_;  //incoming conn
            itf.ioFlush    = TNLyrSsl_ssl_ioFlush_;
            itf.ioShutdown = TNLyrSsl_ssl_ioShutdown_;
            itf.ioClose    = TNLyrSsl_ssl_ioClose_;
            //
            if(!NBSsl_createWithIOLnkItf(ssl, ctx, &itf, opq)){
                PRINTF_ERROR("TNLyrSsl, initial NBSsl_createWithIOLnkItf failed.\n");
                NBStopFlag_activate(opq->stopFlag);
                r = FALSE;
            } else {
                opq->ssl.handshake.state = ENTNLyrSslCmdState_Unknown;
                //consume
                NBSsl_set(&opq->ssl.conn, &ssl);
                NBSsl_set(&opq->ssl.ctx, &ctx);
                opq->flow = ENTNLyrFlow_FromUp;
            }
            //release (if not consumed)
            if(NBSsl_isSet(ssl)){
                NBSsl_release(&ssl);
                NBSsl_null(&ssl);
            }
        }
        //copy CAs
        if(r){
            //empty
            {
                SI32 i; for(i = 0; i < opq->ssl.CAs.use; i++){
                    STNBX509* ca = NBArray_itmPtrAtIndex(&opq->ssl.CAs, STNBX509, i);
                    NBX509_release(ca);
                }
                NBArray_empty(&opq->ssl.CAs);
            }
            //refill
            if(CAs != NULL && CAsSz > 0){
                UI32 i; for(i = 0 ; i < CAsSz && r; i++){
                    STNBX509* src = &CAs[i];
                    STNBX509 ca;
                    NBX509_init(&ca);
                    if(!NBX509_createFromOther(&ca, src)){
                        PRINTF_ERROR("TNLyrSsl, NBX509_createFromOther failed.\n");
                        NBX509_release(&ca);
                        r = FALSE;
                    } else {
                        NBArray_addValue(&opq->ssl.CAs, ca);
                    }
                }
            }
        }
        //apply
        if(r){
            opq->lyr.io.toMe.lnk.itf.ioRead     = TNLyrSsl_ioRead_fromAcceptedConn_;    //incoming conn
            opq->lyr.io.toMe.lnk.itf.ioWrite    = TNLyrSsl_ioWrite_toAcceptedConn_;     //incoming conn
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrSsl_startConnect(STTNLyrSslRef ref, STNBSslContextRef ctx, STNBX509* CAs, const UI32 CAsSz){
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)ref.opaque; NBASSERT(TNLyrSsl_isClass(ref))
    if(NBSslContext_isSet(ctx) && NBSslContext_isCreated(ctx)){
        NBObject_lock(opq);
        r = TRUE;
        NBASSERT(opq->ssl.handshake.state == ENTNLyrSslCmdState_Unknown)
        //buffs
        if(r){
            //prv
            if(!TNBuffs_create(&opq->lyr.prv.buffs.read, TN_CORE_CONN_BUFF_SZ)){ //used only if outgoing flow
                PRINTF_ERROR("TNLyrSsl, TNBuffs_create(prv/read, %d bytes) failed.\n", TN_CORE_CONN_BUFF_SZ);
                r = FALSE;
            }
            if(!TNBuffs_create(&opq->lyr.prv.buffs.write, TN_CORE_CONN_BUFF_SZ)){ //used only if outgoing flow
                PRINTF_ERROR("TNLyrSsl, TNBuffs_create(prv/write, %d bytes) failed.\n", TN_CORE_CONN_BUFF_SZ);
                r = FALSE;
            }
            //nxt
            if(!TNBuffs_create(&opq->lyr.nxt.buffs.read, TN_CORE_CONN_BUFF_SZ)){ //used only if outgoing flow
                PRINTF_ERROR("TNLyrSsl, TNBuffs_create(nxt/read, %d bytes) failed.\n", TN_CORE_CONN_BUFF_SZ);
                r = FALSE;
            }
            if(!TNBuffs_create(&opq->lyr.nxt.buffs.write, TN_CORE_CONN_BUFF_SZ)){ //used only if outgoing flow
                PRINTF_ERROR("TNLyrSsl, TNBuffs_create(nxt/write, %d bytes) failed.\n", TN_CORE_CONN_BUFF_SZ);
                r = FALSE;
            }
        }
        //ssl
        if(r){
            STNBSslRef ssl = NBSsl_alloc(NULL);
            STNBIOLnkItf itf;
            NBMemory_setZeroSt(itf, STNBIOLnkItf);
            //no-lock required
            itf.ioRetain   = NULL; //NBObjRef_retainOpq;
            itf.ioRelease  = NULL; //NBObjRef_releaseOpq;
            //
            itf.ioIsObjRef = NULL;
            //
            itf.ioRead     = TNLyrSsl_ssl_ioRead_fromNext_; //outgoing conn
            itf.ioWrite    = TNLyrSsl_ssl_ioWrite_toNext_;  //outgoing conn
            itf.ioFlush    = TNLyrSsl_ssl_ioFlush_;
            itf.ioShutdown = TNLyrSsl_ssl_ioShutdown_;
            itf.ioClose    = TNLyrSsl_ssl_ioClose_;
            //
            if(!NBSsl_createWithIOLnkItf(ssl, ctx, &itf, opq)){
                PRINTF_ERROR("TNLyrSsl, initial NBSsl_createWithIOLnkItf failed.\n");
                NBStopFlag_activate(opq->stopFlag);
                r = FALSE;
            } else {
                opq->ssl.handshake.state = ENTNLyrSslCmdState_Unknown;
                //consume
                NBSsl_set(&opq->ssl.conn, &ssl);
                NBSsl_set(&opq->ssl.ctx, &ctx);
                opq->flow = ENTNLyrFlow_FromLwr;
            }
            //release (if not consumed)
            if(NBSsl_isSet(ssl)){
                NBSsl_release(&ssl);
                NBSsl_null(&ssl);
            }
        }
        //copy CAs
        if(r){
            //empty
            {
                SI32 i; for(i = 0; i < opq->ssl.CAs.use; i++){
                    STNBX509* ca = NBArray_itmPtrAtIndex(&opq->ssl.CAs, STNBX509, i);
                    NBX509_release(ca);
                }
                NBArray_empty(&opq->ssl.CAs);
            }
            //refill
            if(CAs != NULL && CAsSz > 0){
                UI32 i; for(i = 0 ; i < CAsSz && r; i++){
                    STNBX509* src = &CAs[i];
                    STNBX509 ca;
                    NBX509_init(&ca);
                    if(!NBX509_createFromOther(&ca, src)){
                        PRINTF_ERROR("TNLyrSsl, NBX509_createFromOther failed.\n");
                        NBX509_release(&ca);
                        r = FALSE;
                    } else {
                        NBArray_addValue(&opq->ssl.CAs, ca);
                    }
                }
            }
        }
        //apply
        if(r){
            opq->lyr.io.toMe.lnk.itf.ioRead     = TNLyrSsl_ioRead_fromConnectConn_; //outgoing conn
            opq->lyr.io.toMe.lnk.itf.ioWrite    = TNLyrSsl_ioWrite_toConnectConn_;  //outgoing conn
        }
        NBObject_unlock(opq);
    }
    return r;
}

//lyr chain

BOOL TNLyrSsl_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData){ //set next lyr iterface
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
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
        {
            //apply
            toRelease = opq->lyr.nxt.lstnr;
            opq->lyr.nxt.lstnr = *nxt;
            r = TRUE;
        }
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

void TNLyrSsl_lyrStart_(void* usrData){    //cleanup must wait
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        NBASSERT(FALSE) //ToDo: implement
    }
    NBObject_unlock(opq);
}

BOOL TNLyrSsl_lyrIsRunning_(void* usrData){    //cleanup must wait
    BOOL r = FALSE;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        r = (opq->lyr.nxt.lstnr.itf.lyrIsRunning != NULL && (*opq->lyr.nxt.lstnr.itf.lyrIsRunning)(opq->lyr.nxt.lstnr.usrParam));
    }
    NBObject_unlock(opq);
    return r;
}

void TNLyrSsl_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData){
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    if(lnk == NULL){
        PRINTF_ERROR("TNLyrSsl, TNLyrSsl_lyrConsumeMask_, lnk is NULL.\n");
        NBStopFlag_activate(opq->stopFlag);
    } else if(!NBSsl_isSet(opq->ssl.conn)){
        PRINTF_ERROR("TNLyrSsl, TNLyrSsl_lyrConsumeMask_, ssl is NULL.\n");
        NBStopFlag_activate(opq->stopFlag);
    } else {
        STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData;
        IF_NBASSERT(STNBTimestampMicro timeLastAction = NBTimestampMicro_getUTC(), timeCur; SI64 usDiff;)
        //
        opq->lyr.io.toPrev.isEnabled = TRUE;
        opq->lyr.io.toPrev.lnk = *lnk;
        //error
        if(pollMask & ENNBIOPollsterOpBits_ErrOrGone){
            //stop
            PRINTF_ERROR("TNLyrSsl, conn-error-or-gone.\n");
            NBStopFlag_activate(opq->stopFlag);
        }
        //pollMask
        opq->lyr.prv.pollMask = pollMask;
        //cycle
        {
            UI8 pollReqNxt = 0;
            BOOL anyActionDone = FALSE;
            do {
                anyActionDone = FALSE;
                //complete accept/connect action
                if(opq->ssl.handshake.state != ENTNLyrSslCmdState_Completed && !NBStopFlag_isMineActivated(opq->stopFlag)){
                    if(
                       opq->ssl.handshake.state == ENTNLyrSslCmdState_Unknown //untriggered-yet
                       || ((opq->lyr.prv.pollMask & ENNBIOPollsterOpBit_Read) && opq->ssl.handshake.state == ENTNLyrSslCmdState_ReadHungry) //continue handshake-cmd after read-hungry
                       || ((opq->lyr.prv.pollMask & ENNBIOPollsterOpBit_Write) && opq->ssl.handshake.state == ENTNLyrSslCmdState_WriteHungry) //continue handshake-cmd after write-hungry
                    )
                    {
                        NBASSERT(NBSsl_isSet(opq->ssl.conn))
                        NBASSERT(opq->ssl.handshake.state != ENTNLyrSslCmdState_Completed)
                        switch(opq->flow){
                            case ENTNLyrFlow_FromLwr:
                                {
                                    const ENNBSslResult r = NBSsl_connectHandshake(opq->ssl.conn);
                                    switch (r) {
                                        case ENNBSslResult_Success:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_Completed;
                                            {
                                                opq->ssl.cert.isSignedByCA = FALSE;
                                                NBX509_release(&opq->ssl.cert.cur);
                                                NBX509_init(&opq->ssl.cert.cur);
                                                if(NBSsl_getPeerCertificate(opq->ssl.conn, &opq->ssl.cert.cur)){
                                                    if(NBX509_isCreated(&opq->ssl.cert.cur)){
                                                        SI32 i; for(i = 0 ; i < opq->ssl.CAs.use; i++){
                                                            STNBX509* ca = NBArray_itmPtrAtIndex(&opq->ssl.CAs, STNBX509, i);
                                                            if(NBX509_isCreated(ca) && NBX509_isSignedBy(&opq->ssl.cert.cur, ca)){
                                                                opq->ssl.cert.isSignedByCA = TRUE;
                                                                break;
                                                            }
                                                        }
                                                    }
                                                    PRINTF_INFO("TNLyrSsl, NBSsl_connect completed with peer providing a certificate (%s-by-CA).\n", opq->ssl.cert.isSignedByCA ? "signed" : "not-signed");
                                                } else {
                                                    PRINTF_INFO("TNLyrSsl, NBSsl_connect completed with peer NOT providing a certificate.\n");
                                                }
                                            }
                                            break;
                                        case ENNBSslResult_ErrWantRead:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_ReadHungry;
                                            //PRINTF_INFO("TNLyrSsl, NBSsl_accept hungry-for-read.\n");
                                            break;
                                        case ENNBSslResult_ErrWantWrite:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_WriteHungry;
                                            //PRINTF_INFO("TNLyrSsl, NBSsl_accept hungry-for-write.\n");
                                            break;
                                        default:
                                            if(NBStopFlag_isSet(opq->stopFlag)){
                                                NBStopFlag_activate(opq->stopFlag);
                                            }
                                            break;
                                    }
                                }
                                break;
                            case ENTNLyrFlow_FromUp:
                                {
                                    const ENNBSslResult r = NBSsl_accept(opq->ssl.conn);
                                    switch (r) {
                                        case ENNBSslResult_Success:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_Completed;
                                            {
                                                opq->ssl.cert.isSignedByCA = FALSE;
                                                NBX509_release(&opq->ssl.cert.cur);
                                                NBX509_init(&opq->ssl.cert.cur);
                                                if(NBSsl_getPeerCertificate(opq->ssl.conn, &opq->ssl.cert.cur)){
                                                    if(NBX509_isCreated(&opq->ssl.cert.cur)){
                                                        SI32 i; for(i = 0 ; i < opq->ssl.CAs.use; i++){
                                                            STNBX509* ca = NBArray_itmPtrAtIndex(&opq->ssl.CAs, STNBX509, i);
                                                            if(NBX509_isCreated(ca) && NBX509_isSignedBy(&opq->ssl.cert.cur, ca)){
                                                                opq->ssl.cert.isSignedByCA = TRUE;
                                                                break;
                                                            }
                                                        }
                                                    }
                                                    PRINTF_INFO("TNLyrSsl, NBSsl_accept completed with peer providing a certificate (%s-by-CA).\n", opq->ssl.cert.isSignedByCA ? "signed" : "not-signed");
                                                } else {
                                                    PRINTF_INFO("TNLyrSsl, NBSsl_accept completed with peer NOT providing a certificate.\n");
                                                }
                                            }
                                            break;
                                        case ENNBSslResult_ErrWantRead:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_ReadHungry;
                                            //PRINTF_INFO("TNLyrSsl, NBSsl_accept hungry-for-read.\n");
                                            break;
                                        case ENNBSslResult_ErrWantWrite:
                                            opq->ssl.handshake.state = ENTNLyrSslCmdState_WriteHungry;
                                            //PRINTF_INFO("TNLyrSsl, NBSsl_accept hungry-for-write.\n");
                                            break;
                                        default:
                                            if(NBStopFlag_isSet(opq->stopFlag)){
                                                NBStopFlag_activate(opq->stopFlag);
                                            }
                                            break;
                                    }
                                }
                                break;
                            default:
                                NBASSERT(FALSE) //unimplemented
                                break;
                        }
                    }
                }
                //outgoing connection
                if(opq->flow == ENTNLyrFlow_FromLwr && !NBStopFlag_isMineActivated(opq->stopFlag)){
                    
                }
                //notify nxt lyr
                if(opq->lyr.nxt.lstnr.itf.lyrConsumeMask != NULL && !NBStopFlag_isMineActivated(opq->stopFlag)){
                    NBASSERT(!opq->lyr.io.toMe.lnkIsEnabled)
                    opq->lyr.io.toMe.lnkIsEnabled = TRUE;
                    {
                        const UI8 pollMaskNxt = ENNBIOPollsterOpBit_Read | ENNBIOPollsterOpBit_Write;
                        pollReqNxt = 0;
                        (*opq->lyr.nxt.lstnr.itf.lyrConsumeMask)(&opq->lyr.io.toMe.lnk, pollMaskNxt, &pollReqNxt, opq->lyr.nxt.lstnr.usrParam);
                    }
                    opq->lyr.io.toMe.lnkIsEnabled = FALSE;
                }
            } while(anyActionDone && !NBStopFlag_isMineActivated(opq->stopFlag));
            //return
            if(dstPollReq != NULL){
                switch(opq->flow){
                    case ENTNLyrFlow_FromLwr:
                        //ToDo: implement
                        break;
                    case ENTNLyrFlow_FromUp:
                        *dstPollReq = (opq->ssl.handshake.state == ENTNLyrSslCmdState_Unknown || opq->ssl.handshake.state == ENTNLyrSslCmdState_ReadHungry || opq->ssl.cmd.state == ENTNLyrSslCmdState_ReadHungry ? ENNBIOPollsterOpBit_Read : 0) | (opq->ssl.handshake.state == ENTNLyrSslCmdState_WriteHungry || opq->ssl.cmd.state == ENTNLyrSslCmdState_WriteHungry ? ENNBIOPollsterOpBit_Write : 0) | pollReqNxt;
                        break;
                    default:
                        NBASSERT(FALSE) //unimplemented
                        break;
                }
            }
        }
        //print-tick-time
#       ifdef NB_CONFIG_INCLUDE_ASSERTS
        {
            timeCur = NBTimestampMicro_getUTC();
            usDiff  = NBTimestampMicro_getDiffInUs(&timeLastAction, &timeCur);
            /*if(usDiff >= 1000ULL){
                PRINTF_INFO("TNLyrSsl, pollConsumeMask(%s%s%s) took %llu.%llu%llums.\n", (pollMask & ENNBIOPollsterOpBit_Read ? "+read" : ""), (pollMask & ENNBIOPollsterOpBit_Write ? "+write" : ""), (pollMask & ENNBIOPollsterOpBits_ErrOrGone ? "+errOrGone" : ""), (usDiff / 1000ULL), (usDiff % 1000ULL) % 100ULL, (usDiff % 100ULL) % 10ULL);
            }*/
            timeLastAction = timeCur;
        }
#       endif
        opq->lyr.io.toPrev.isEnabled = FALSE;
    }
    //notify stopFlag to lower-layer(s)
    if(NBStopFlag_isMineActivated(opq->stopFlag)){
        if(lnk != NULL && lnk->itf.ioClose != NULL){
            (*lnk->itf.ioClose)(lnk->usrData);
        }
    }
    NBObject_unlock(opq);
}


void TNLyrSsl_lyrShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        //
    }
    NBObject_unlock(opq);
}

void TNLyrSsl_lyrClose_(void* usrData){
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        //my-flag
        NBStopFlag_activate(opq->stopFlag);
        //up-lyr
        if(opq->lyr.nxt.lstnr.itf.lyrClose != NULL){
            (*opq->lyr.nxt.lstnr.itf.lyrClose)(opq->lyr.nxt.lstnr.usrParam);
        }
    }
    NBObject_unlock(opq);
}

void TNLyrSsl_lyrConcat_(STNBString* dst, void* usrData) {
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    if (dst != NULL) {
        NBObject_lock(opq);
        {
            NBASSERT(FALSE) //ToDo: implement
            /*NBString_concat(dst, "TNLyrSsl (");
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
            }*/
        }
        NBObject_unlock(opq);
    }
}

//NBIOLink

SI32 TNLyrSsl_ssl_ioRead_fromPrev_(void* dst, const SI32 dstSz, void* usrData){ //incoming conn
    SI32 r = NB_IO_ERROR;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBASSERT(opq->lyr.io.toPrev.isEnabled) //Must be enabled
    if(opq->lyr.io.toPrev.isEnabled && !NBStopFlag_isMineActivated(opq->stopFlag) && dst != NULL && dstSz >= 0){
        if(opq->lyr.io.toPrev.lnk.itf.ioRead != NULL){
            r = (*opq->lyr.io.toPrev.lnk.itf.ioRead)(dst, dstSz, opq->lyr.io.toPrev.lnk.usrData);
            //disable read (nothing returned)
            if(r <= 0){
                opq->lyr.prv.pollMask &= ~ENNBIOPollsterOpBit_Read;
            }
        }
    }
    return r;
}

SI32 TNLyrSsl_ssl_ioWrite_toPrev_(const void* pSrc, const SI32 srcSz, void* usrData){ //incoming conn
    SI32 r = NB_IO_ERROR;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    NBASSERT(opq->lyr.io.toPrev.isEnabled) //Must be enabled
    if(opq->lyr.io.toPrev.isEnabled && !NBStopFlag_isMineActivated(opq->stopFlag)){
        if(opq->lyr.io.toPrev.lnk.itf.ioWrite != NULL){
            r = (*opq->lyr.io.toPrev.lnk.itf.ioWrite)(pSrc, srcSz, opq->lyr.io.toPrev.lnk.usrData);
            //disable write (not all data was written)
            if(r < srcSz){
                opq->lyr.prv.pollMask &= ~ENNBIOPollsterOpBit_Write;
            }
        }
    }
    return r;
}

SI32 TNLyrSsl_ssl_ioRead_fromNext_(void* pDst, const SI32 dstSz, void* usrData){    //outgoing conn
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    STTNBuffs* buffs = &opq->lyr.nxt.buffs.write; //reading from nxt, is done in the write-buffer
    return TNBuffs_consume(buffs, pDst, dstSz);
}

SI32 TNLyrSsl_ssl_ioWrite_toNext_(const void* pSrc, const SI32 srcSz, void* usrData){   //outgoing conn
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    STTNBuffs* buffs = &opq->lyr.nxt.buffs.read;
    return TNBuffs_fill(buffs, pSrc, srcSz);
}

void TNLyrSsl_ssl_ioFlush_(void* usrData){ //close gracefully (if posible)
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    {
        NBASSERT(FALSE) //ToDo: implement
    }
}

void TNLyrSsl_ssl_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    {
        NBStopFlag_activate(opq->stopFlag);
    }
}

void TNLyrSsl_ssl_ioClose_(void* usrData){ //close ungracefully
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    {
        NBStopFlag_activate(opq->stopFlag);
    }
}

//io

SI32 TNLyrSsl_ioRead_fromAcceptedConn_(void* pDst, const SI32 dstSz, void* usrData){ //incoming conn
    SI32 r = -1;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.io.toPrev.isEnabled) //this should be called only inside 'nxt.consumeMask' call
    NBASSERT(opq->lyr.io.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.io.toMe.lnkIsEnabled && dstSz >= 0){
        if(opq->ssl.handshake.state != ENTNLyrSslCmdState_Completed){
            //waiting for accept to complete
            r = 0;
        } else {
            const SI32 rcvd = NBSsl_read(opq->ssl.conn, pDst, dstSz);
            //process
            if(rcvd >= 0){
                //PRINTF_INFO("TNLyrSsl, NBSsl_read received %d bytes.\n", rcvd);
                opq->ssl.cmd.state = ENTNLyrSslCmdState_Completed;
                r = rcvd;
            } else {
                //results
                switch (rcvd) {
                    case ENNBSslResult_ErrWantRead:
                        PRINTF_INFO("TNLyrSsl, NBSsl_read hungry-for-read.\n");
                        opq->ssl.cmd.state = ENTNLyrSslCmdState_ReadHungry;
                        r = 0;
                        break;
                    case ENNBSslResult_ErrWantWrite:
                        PRINTF_INFO("TNLyrSsl, NBSsl_read hungry-for-write.\n");
                        opq->ssl.cmd.state = ENTNLyrSslCmdState_WriteHungry;
                        r = 0;
                        break;
                    default:
                        PRINTF_ERROR("TNLyrSsl, NBSsl_read failed with (%d) plain.\n", rcvd);
                        NBASSERT(rcvd == ENNBSslResult_Error)
                        if(NBStopFlag_isSet(opq->stopFlag)){
                            NBStopFlag_activate(opq->stopFlag);
                        }
                        break;
                }
            }
        }
    }
    return r;
}

SI32 TNLyrSsl_ioWrite_toAcceptedConn_(const void* pSrc, const SI32 srcSz, void* usrData){   //incoming conn
    SI32 r = -1;
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.io.toPrev.isEnabled) //this should be called only inside 'nxt.consumeMask' call
    NBASSERT(opq->lyr.io.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.io.toMe.lnkIsEnabled && srcSz >= 0){
        if(opq->ssl.handshake.state != ENTNLyrSslCmdState_Completed){
            //waiting for accept to complete
            r = 0;
        } else {
            const SI32 sent = NBSsl_write(opq->ssl.conn, pSrc, srcSz);
            //process
            if(sent >= 0){
                //PRINTF_INFO("TNLyrSsl, NBSsl_read received %d bytes.\n", rcvd);
                opq->ssl.cmd.state = ENTNLyrSslCmdState_Completed;
                r = sent;
            } else {
                //results
                switch (sent) {
                    case ENNBSslResult_ErrWantRead:
                        PRINTF_INFO("TNLyrSsl, NBSsl_write hungry-for-read.\n");
                        opq->ssl.cmd.state = ENTNLyrSslCmdState_ReadHungry;
                        r = 0;
                        break;
                    case ENNBSslResult_ErrWantWrite:
                        PRINTF_INFO("TNLyrSsl, NBSsl_write hungry-for-write.\n");
                        opq->ssl.cmd.state = ENTNLyrSslCmdState_WriteHungry;
                        r = 0;
                        break;
                    default:
                        PRINTF_ERROR("TNLyrSsl, NBSsl_write failed with (%d).\n", sent);
                        NBASSERT(sent == ENNBSslResult_Error)
                        if(NBStopFlag_isSet(opq->stopFlag)){
                            NBStopFlag_activate(opq->stopFlag);
                        }
                        break;
                }
            }
        }
    }
    return r;
}

SI32 TNLyrSsl_ioRead_fromConnectConn_(void* pDst, const SI32 dstSz, void* usrData) { //outgoing conn
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    STTNBuffs* buffs = &opq->lyr.nxt.buffs.read;
    return TNBuffs_consume(buffs, pDst, dstSz);
}

SI32 TNLyrSsl_ioWrite_toConnectConn_(const void* pSrc, const SI32 srcSz, void* usrData){ //outgoing conn
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    STTNBuffs* buffs = &opq->lyr.nxt.buffs.write;
    return TNBuffs_fill(buffs, pSrc, srcSz);
}

void TNLyrSsl_ioFlush_(void* usrData){ //close gracefully (if posible)
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.io.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.io.toMe.lnkIsEnabled){
        NBASSERT(FALSE) //ToDo: implement
    }
}

void TNLyrSsl_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.io.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.io.toMe.lnkIsEnabled){
        NBStopFlag_activate(opq->stopFlag);
    }
}

void TNLyrSsl_ioClose_(void* usrData){ //close ungracefully
    STTNLyrSslOpq* opq = (STTNLyrSslOpq*)usrData; NBASSERT(TNLyrSsl_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.io.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.io.toMe.lnkIsEnabled){
        NBStopFlag_activate(opq->stopFlag);
    }
}
