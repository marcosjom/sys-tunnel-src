//
//  TNLyrIO.c
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
#include "core/TNLyrIO.h"

//TNLyrIO

typedef struct STTNLyrIOOpq_ {
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
                STTNLyrLstnr    lstnr;
            } toUp;
            //toLwr
            struct {
                BOOL        isEnabled;      //link is only allowed to be called during 'nxt.consumeMask' call
                STNBIOLnk   lnk;            //link to prev layer sent to me
                //read
                struct {
                    BOOL    isBlocked;      //last action retruned zero results, flag reseteable.
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
                } read;
                //write
                struct {
                    BOOL    isBlocked;      //last action retruned zero results, flag reseteable.
                    BOOL    isShuttedDown;  //no more action can be done, flag is permanent.
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
    //pollster
    struct {
        STNBIOPollsterSyncRef sync;         //default
        STNBIOPollstersProviderRef provider; //provider
        BOOL                isListening;
    } pollster;
    //io (socket/file)
    struct {
        STNBSocketRef       socket;         //net-socket
        STNBFileRef         file;           //io-file
        BOOL                isDuplex;       //can send and receive on the opened object (socket/file/...)
        BOOL                ignoreEof;      //TRUE, if new data is expected to be written after EOF was read.
        //hanshake
        struct {
            STNBTimestampMicro startTime;
            BOOL            isCompleted;
        } handshake;
        //read
        struct {
            STTNBuffs*      buffs;          //pointer to toUpLyr or fromUpLyr, using pointer to reduce conditionals usage
            BOOL            isBlocked;      //last action retruned zero results, flag reseteable.
            BOOL            isShuttedDown;  //no more action can be done, flag is permanent.
            BOOL            isShutNotified; //shutdown notified to upperLyr (if incoming flow) or lwrLyr (if outgoing flow).
        } read;
        //write
        struct {
            STTNBuffs*      buffs;          //pointer to toUpLyr or fromUpLyr, using pointer to reduce conditionals usage
            BOOL            isBlocked;      //last action retruned zero results, flag reseteable.
            BOOL            isShuttedDown;  //no more action can be done, flag is permanent.
            BOOL            isShutNotified; //shutdown notified to upperLyr (if incoming flow) or lwrLyr (if outgoing flow).
        } write;
        //buffs
        struct {
            STTNBuffs       toUpLyr;        //for read-ops coming from upper-lyr
            STTNBuffs       fromUpLyr;      //for write-ops coming from upper-lyr
        } buffs;
    } io;
} STTNLyrIOOpq;

//

NB_OBJREF_BODY(TNLyrIO, STTNLyrIOOpq, NBObject)

//stops reading after stopFlag is activated.
//continues writting after stopFlag was activated (to flush if posible).
#define TNLyrIO_ioIsCfgAsRedeable(OPQ)              ((OPQ)->io.isDuplex || (OPQ)->flow == ENTNLyrFlow_FromUp)
#define TNLyrIO_ioIsCfgAsWriteable(OPQ)             ((OPQ)->io.isDuplex || (OPQ)->flow == ENTNLyrFlow_FromLwr)
//
#define TNLyrIO_ioGetPollMask(OPQ)                  (!(OPQ)->io.handshake.isCompleted || (!(OPQ)->io.read.isShuttedDown && TNBuffs_canFill((OPQ)->io.read.buffs) && !(OPQ)->lyr.fromMe.toLwr.write.isShuttedDown /*flag-activates-only-for-outgoing-flows*/) ? ENNBIOPollsterOpBit_Read : 0) | (!(OPQ)->io.handshake.isCompleted || (!(OPQ)->io.write.isShuttedDown && TNBuffs_canConsume((OPQ)->io.write.buffs)) ? ENNBIOPollsterOpBit_Write : 0)
#define TNLyrIO_ioBuffsIsShuttedDown(OPQ, BUFFS)    (((BUFFS) == (OPQ)->io.read.buffs && (OPQ)->io.read.isShuttedDown) || ((BUFFS) == (OPQ)->io.write.buffs && (OPQ)->io.write.isShuttedDown))
//
#define TNLyrIO_imBussy(OPQ)                        (!(OPQ)->io.read.isShuttedDown || !(OPQ)->io.read.isShutNotified || !(OPQ)->io.write.isShuttedDown || !(OPQ)->io.write.isShutNotified)
//

void TNLyrIO_pollConsumeMask_(STNBIOLnk ioLnk, const UI8 pollMask, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrIO_pollConsumeNoOp_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData); //consume non-ops and return the ones that are required to poll.
void TNLyrIO_pollGetReqUpd_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, void* usrData); //optional, if the client expects to change the requested 'opsMasks' outside the 'pollConsumeMask' or 'pollConsumeNoOp' calls.
void TNLyrIO_pollRemoved_(STNBIOLnk ioLnk, void* usrData);

//

SI32 TNLyrIO_ioRead_(void* dst, const SI32 dstSz, void* usrData); //read data to destination buffer, returns the ammount of bytes read, negative in case of error
SI32 TNLyrIO_ioWrite_(const void* src, const SI32 srcSz, void* usrData); //write data from source buffer, returns the ammount of bytes written, negative in case of error
void TNLyrIO_ioFlush_(void* usrData);       //flush write-data
void TNLyrIO_ioShutdown_(const UI8 mask, void* usrData);    //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrIO_ioClose_(void* usrData);       //close ungracefully

//

void TNLyrIO_initZeroed(STNBObject* obj){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)obj;
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
            opq->lyr.toMe.lnk.itf.ioRead     = TNLyrIO_ioRead_;
            opq->lyr.toMe.lnk.itf.ioWrite    = TNLyrIO_ioWrite_;
            opq->lyr.toMe.lnk.itf.ioFlush    = TNLyrIO_ioFlush_;
            opq->lyr.toMe.lnk.itf.ioShutdown = TNLyrIO_ioShutdown_;
            opq->lyr.toMe.lnk.itf.ioClose    = TNLyrIO_ioClose_;
        }
    }
    //net
    {
        //
    }
    //io
    {
        //buffs
        {
            TNBuffs_init(&opq->io.buffs.toUpLyr);
            TNBuffs_init(&opq->io.buffs.fromUpLyr);
            //
            TNBuffs_create(&opq->io.buffs.toUpLyr, TN_CORE_CONN_BUFF_SZ);
            TNBuffs_create(&opq->io.buffs.fromUpLyr, TN_CORE_CONN_BUFF_SZ);
        }
    }
}

void TNLyrIO_uninitLocked(STNBObject* obj){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)obj;
    //activate stop-flag
    NBStopFlag_activate(opq->stopFlag);
    //lyr
    {
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrRelease != NULL){
            (*opq->lyr.fromMe.toUp.lstnr.itf.lyrRelease)(opq->lyr.fromMe.toUp.lstnr.usrParam);
        }
        NBMemory_setZeroSt(opq->lyr.fromMe.toUp.lstnr, STTNLyrLstnr);
    }
    //pollster
    {
        //wait for io removal from pollster
        while(opq->pollster.isListening){
            NBThreadCond_timedWaitObj(&opq->cond, opq, 100);
        }
        //
        if(NBIOPollsterSync_isSet(opq->pollster.sync)){
            NBIOPollsterSync_release(&opq->pollster.sync);
            NBIOPollsterSync_null(&opq->pollster.sync);
        }
        if(NBIOPollstersProvider_isSet(opq->pollster.provider)){
            NBIOPollstersProvider_release(&opq->pollster.provider);
            NBIOPollstersProvider_null(&opq->pollster.provider);
        }
    }
    //io
    {
        //socket
        if(NBSocket_isSet(opq->io.socket)){
            NBSocket_release(&opq->io.socket);
            NBSocket_null(&opq->io.socket);
        }
        //file
        if(NBFile_isSet(opq->io.file)){
            NBFile_release(&opq->io.file);
            NBFile_null(&opq->io.file);
        }
        //buffs
        //read
        opq->io.read.buffs = NULL;
        opq->io.write.buffs = NULL;
        TNBuffs_release(&opq->io.buffs.toUpLyr);
        TNBuffs_release(&opq->io.buffs.fromUpLyr);
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
BOOL TNLyrIO_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData); //set next lyr iterface
//data flow
void TNLyrIO_lyrStart_(void* usrData);
BOOL TNLyrIO_lyrIsRunning_(void* usrData);    //cleanup must wait
void TNLyrIO_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData); //consume operations and return the ones that are required to poll.
void TNLyrIO_lyrShutdown_(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
void TNLyrIO_lyrClose_(void* usrData);
//dbg
void TNLyrIO_lyrConcat_(STNBString* dst, void* usrData);
//

BOOL TNLyrIO_getLyrItf(STTNLyrIORef ref, STTNLyrLstnr* dst){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    if(dst != NULL){
        NBObject_lock(opq);
        {
            NBMemory_setZeroSt(dst->itf, STTNLyrLstnrItf);
            dst->itf.lyrRetain     = NBObjRef_retainOpq;
            dst->itf.lyrRelease    = NBObjRef_releaseOpq;
            //lyr chain
            dst->itf.lyrSetNext    = TNLyrIO_lyrSetNext_; //set next lyr iterface
            //data flow
            dst->itf.lyrStart       = TNLyrIO_lyrStart_;
            dst->itf.lyrIsRunning   = TNLyrIO_lyrIsRunning_; //cleanup must wait
            dst->itf.lyrConsumeMask = TNLyrIO_lyrConsumeMask_;
            dst->itf.lyrShutdown    = TNLyrIO_lyrShutdown_;
            dst->itf.lyrClose       = TNLyrIO_lyrClose_;
            //dbg
            dst->itf.lyrConcat      = TNLyrIO_lyrConcat_;
            //
            dst->usrParam           = opq;
            r = TRUE;
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrIO_setPollsterSync(STTNLyrIORef ref, STNBIOPollsterSyncRef pollSync){    //when one pollster only
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    NBASSERT(opq != NULL)
    NBObject_lock(opq);
    if(!opq->pollster.isListening){
        //set
        NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
        //
        r = TRUE;
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNLyrIO_setPollstersProvider(STTNLyrIORef ref, STNBIOPollstersProviderRef provider){ //when multiple pollsters
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    NBASSERT(opq != NULL)
    NBObject_lock(opq);
    if(!opq->pollster.isListening){
        //set
        NBIOPollstersProvider_set(&opq->pollster.provider, &provider);
        //
        r = TRUE;
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNLyrIO_setParentStopFlag(STTNLyrIORef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
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

//socket

BOOL TNLyrIO_prepareOwningAcceptedSocket(STTNLyrIORef ref, STNBSocketRef socket){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    if(NBSocket_isSet(socket)){
        NBObject_lock(opq);
        if(!opq->isPrepared){
            NBASSERT(!opq->io.handshake.isCompleted)
            r = TRUE;
            //pollster
            {
                //get a pollster from provider (if necesary)
                if(!NBIOPollsterSync_isSet(opq->pollster.sync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                    STNBIOPollsterSyncRef pollSync =  NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                    if(NBIOPollsterSync_isSet(pollSync)){
                        NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
                    }
                }
                //
                if(!NBIOPollsterSync_isSet(opq->pollster.sync)){
                    PRINTF_ERROR("TNLyrIO, could not define a pollster.\n");
                    r = FALSE;
                }
            }
            //set socket
            if(r){
                opq->flow               = ENTNLyrFlow_FromUp;
                opq->isPrepared         = TRUE;
                opq->io.isDuplex        = TRUE;
                opq->io.ignoreEof       = FALSE; //stop-at-eof
                opq->io.read.buffs      = &opq->io.buffs.toUpLyr;     //receive data and let up-layer to read it
                opq->io.write.buffs     = &opq->io.buffs.fromUpLyr;    //send data provided from up-layer
                opq->io.handshake.startTime = NBTimestampMicro_getUTC();
                opq->io.handshake.isCompleted = TRUE;
                NBFile_release(&opq->io.file);
                NBFile_null(&opq->io.file);
                NBSocket_set(&opq->io.socket, &socket);
                NBSocket_setNoSIGPIPE(opq->io.socket, TRUE);
                //NBSocket_setCorkEnabled(opq->io.socket, FALSE);
                //NBSocket_setDelayEnabled(opq->io.socket, FALSE);
                NBSocket_setNonBlocking(opq->io.socket, TRUE);
                NBSocket_setUnsafeMode(opq->io.socket, TRUE);
                //apply shutdown by cfg
                opq->io.read.isShuttedDown = !TNLyrIO_ioIsCfgAsRedeable(opq);
                opq->io.write.isShuttedDown = !TNLyrIO_ioIsCfgAsWriteable(opq);
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNLyrIO_prepareConnecting(STTNLyrIORef ref, const char* server, const SI32 port){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    if(!NBString_strIsEmpty(server) && port > 0){
        NBObject_lock(opq);
        if(!opq->isPrepared){
            NBASSERT(!opq->io.handshake.isCompleted)
            STNBSocketRef socket = NBSocket_alloc(NULL);
            r = TRUE;
            //connect
            if(r){
                NBSocket_setNoSIGPIPE(socket, TRUE);
                //NBSocket_setCorkEnabled(socket, FALSE);
                //NBSocket_setDelayEnabled(socket, FALSE);
                NBSocket_setNonBlocking(socket, TRUE);
                NBSocket_setUnsafeMode(socket, TRUE);
                if(!NBSocket_connect(socket, server, port)){
                    PRINTF_ERROR("TNLyrIO, connect('%s', %d) failed.\n", server, port);
                    r = FALSE;
                }
            }
            //pollster
            if(r){
                //get a pollster from provider (if necesary)
                if(!NBIOPollsterSync_isSet(opq->pollster.sync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                    STNBIOPollsterSyncRef pollSync =  NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                    if(NBIOPollsterSync_isSet(pollSync)){
                        NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
                    }
                }
                //
                if(!NBIOPollsterSync_isSet(opq->pollster.sync)){
                    PRINTF_ERROR("TNLyrIO, could not define a pollster.\n");
                    r = FALSE;
                }
            }
            //set socket
            if(r){
                opq->flow               = ENTNLyrFlow_FromLwr;
                opq->isPrepared         = TRUE;
                opq->io.isDuplex        = TRUE;
                opq->io.ignoreEof       = FALSE; //stop-at-eof
                opq->io.read.buffs      = &opq->io.buffs.fromUpLyr;    //receive data to pass layer-down
                opq->io.write.buffs     = &opq->io.buffs.toUpLyr;     //send data read fromlayer-down
                opq->io.handshake.startTime = NBTimestampMicro_getUTC();
                opq->io.handshake.isCompleted = FALSE;
                NBFile_release(&opq->io.file);
                NBFile_null(&opq->io.file);
                NBSocket_set(&opq->io.socket, &socket);
                //apply shutdown by cfg
                opq->io.read.isShuttedDown = !TNLyrIO_ioIsCfgAsRedeable(opq);
                opq->io.write.isShuttedDown = !TNLyrIO_ioIsCfgAsWriteable(opq);
            }
            NBSocket_release(&socket);
            NBSocket_null(&socket);
        }
        NBObject_unlock(opq);
    }
    return r;
}

//file

BOOL TNLyrIO_prepareAsStdIn(STTNLyrIORef ref){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    NBObject_lock(opq);
    if(!opq->isPrepared){
        NBASSERT(!opq->io.handshake.isCompleted)
        STNBFileRef file = NBFile_alloc(NULL);
        if(!NBFile_openAsStd(file, ENNBFileStd_In)){
            PRINTF_ERROR("TNLyrIO, NBFile_openAsStd(TRUE) failed for stdin.\n");
            r = FALSE;
        } else {
            r = TRUE;
            //set non-blocking
            if(r){
                NBFile_lock(file);
                if(!NBFile_setNonBlocking(file, TRUE)){
                    PRINTF_ERROR("TNLyrIO, NBFile_setNonBlocking(TRUE) failed for stdin.\n");
                    r = FALSE;
                }
                NBFile_unlock(file);
            }
            //continue
            if(r){
                //pollster
                {
                    //get a pollster from provider (if necesary)
                    if(!NBIOPollsterSync_isSet(opq->pollster.sync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                        STNBIOPollsterSyncRef pollSync =  NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                        if(NBIOPollsterSync_isSet(pollSync)){
                            NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
                        }
                    }
                    //
                    if(!NBIOPollsterSync_isSet(opq->pollster.sync)){
                        PRINTF_ERROR("TNLyrIO, could not define a pollster.\n");
                        r = FALSE;
                    }
                }
                //set socket
                if(r){
                    opq->flow               = ENTNLyrFlow_FromUp;
                    opq->isPrepared         = TRUE;
                    opq->io.isDuplex        = FALSE; //read-only
                    opq->io.ignoreEof       = FALSE; //stop-at-eof
                    opq->io.read.buffs      = &opq->io.buffs.toUpLyr;     //receive data and let up-layer to read it
                    opq->io.write.buffs     = &opq->io.buffs.fromUpLyr;    //send data provided from up-layer
                    opq->io.handshake.startTime = NBTimestampMicro_getUTC();
                    opq->io.handshake.isCompleted = TRUE;
                    NBSocket_release(&opq->io.socket);
                    NBSocket_null(&opq->io.socket);
                    NBFile_set(&opq->io.file, &file);
                    //apply shutdown by cfg
                    opq->io.read.isShuttedDown = !TNLyrIO_ioIsCfgAsRedeable(opq);
                    opq->io.write.isShuttedDown = !TNLyrIO_ioIsCfgAsWriteable(opq);
                }
            }
        }
        NBFile_release(&file);
        NBFile_null(&file);
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNLyrIO_prepareAsStdOut(STTNLyrIORef ref){
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)ref.opaque; NBASSERT(TNLyrIO_isClass(ref))
    NBObject_lock(opq);
    if(!opq->isPrepared){
        NBASSERT(!opq->io.handshake.isCompleted)
        STNBFileRef file = NBFile_alloc(NULL);
        if(!NBFile_openAsStd(file, ENNBFileStd_Out)){
            PRINTF_ERROR("TNLyrIO, NBFile_openAsStd() failed for stdout.\n");
            r = FALSE;
        } else {
            r = TRUE;
            //set non-blocking
            if(r){
                NBFile_lock(file);
                if(!NBFile_setNonBlocking(file, TRUE)){
                    PRINTF_ERROR("TNLyrIO, NBFile_setNonBlocking(TRUE) failed for stdout.\n");
                    r = FALSE;
                }
                NBFile_unlock(file);
            }
            //continue
            if(r){
                //pollster
                if(r){
                    //get a pollster from provider (if necesary)
                    if(!NBIOPollsterSync_isSet(opq->pollster.sync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                        STNBIOPollsterSyncRef pollSync =  NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                        if(NBIOPollsterSync_isSet(pollSync)){
                            NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
                        }
                    }
                    //
                    if(!NBIOPollsterSync_isSet(opq->pollster.sync)){
                        PRINTF_ERROR("TNLyrIO, could not define a pollster.\n");
                        r = FALSE;
                    }
                }
                //set socket
                if(r){
                    opq->flow               = ENTNLyrFlow_FromLwr;
                    opq->isPrepared         = TRUE;
                    opq->io.isDuplex        = FALSE; //write-only
                    opq->io.ignoreEof       = FALSE; //stop-at-eof
                    opq->io.read.buffs      = &opq->io.buffs.fromUpLyr;   //receive data to pass layer-down
                    opq->io.write.buffs     = &opq->io.buffs.toUpLyr;     //send data read fromlayer-down
                    opq->io.handshake.startTime = NBTimestampMicro_getUTC();
                    opq->io.handshake.isCompleted = FALSE;
                    NBSocket_release(&opq->io.socket);
                    NBSocket_null(&opq->io.socket);
                    NBFile_set(&opq->io.file, &file);
                    //apply shutdown by cfg
                    opq->io.read.isShuttedDown = !TNLyrIO_ioIsCfgAsRedeable(opq);
                    opq->io.write.isShuttedDown = !TNLyrIO_ioIsCfgAsWriteable(opq);
                }
            }
        }
        NBFile_release(&file);
        NBFile_null(&file);
    }
    NBObject_unlock(opq);
    return r;
}

//pollster callbacks

void TNLyrIO_pollReadOpq_(STTNLyrIOOpq* opq, STNBIOLnk* ioLnk){
    IF_NBASSERT(UI32 dbgReadsCount = 0;)
    STTNBuffs* buffs = opq->io.read.buffs;
    //populate read buffer
    //stops reading after stopFlag is activated.
    while(!opq->io.read.isBlocked && TNBuffs_canFill(buffs) && !opq->io.read.isShuttedDown){
        //recv
        const SI32 rcvAvail = (buffs->fill->size - buffs->fill->filled);
        const SI32 rcvd = NBIOLnk_read(ioLnk, &buffs->fill->data[buffs->fill->filled], rcvAvail);
        //process
        if(rcvd < 0){
            if(rcvd == NB_IO_ERR_EOF){
                if(!opq->io.ignoreEof){
                    //stop (IO eof)
                    opq->io.read.isShuttedDown = TRUE;
                    PRINTF_INFO("TNLyrIO, NBIOLnk_read EOF.\n");
                }
            } else {
                //stop (IO error)
                opq->io.read.isShuttedDown = TRUE;
                PRINTF_ERROR("TNLyrIO, NBIOLnk_read failed with (%d).\n", rcvd);
            }
            opq->io.read.isBlocked = TRUE;
            break;
        } else {
            if(rcvd > 0){
                //PRINTF_INFO("TNLyrIO, (%s) NBIOLnk_read received %d bytes.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rcvd);
                /*{
                 STNBString str;
                 NBString_initWithStrBytes(&str, (const char*)&buffs->fill->data[buffs->fill->filled], rcvd);
                 PRINTF_INFO("TNLyrIO, NBIOLnk_read received %d bytes:--->\n%s<---.\n", rcvd, str.str);
                 NBString_release(&str);
                 }*/
                TNBuffs_moveFillCursor(buffs, rcvd);
            }
            //wait for flag to be cleaned
            opq->io.read.isBlocked = (rcvd < rcvAvail);
        }
        IF_NBASSERT(dbgReadsCount++;)
        IF_NBASSERT(if(dbgReadsCount > 3){ PRINTF_WARNING("TNLyrIO, (%s) %d io-reads.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), dbgReadsCount); })
    }
}

void TNLyrIO_pollWriteOpq_(STTNLyrIOOpq* opq, STNBIOLnk* ioLnk){
    //ToDo: voluntarely stop sending after a limit (avoid chance of hijacking the thread on fast network action)
    IF_NBASSERT(UI32 dbgWritesCount = 0;)
    STTNBuffs* buffs = opq->io.write.buffs;
    //consume write buffer
    //continues writting after stopFlag was activated (to flush if posible).
    while(!opq->io.write.isBlocked && TNBuffs_canConsume(buffs) && !opq->io.write.isShuttedDown){
        const SI32 sendAvail = (buffs->read->filled - buffs->read->csmd);
        const SI32 sent = NBIOLnk_write(ioLnk, &buffs->read->data[buffs->read->csmd], sendAvail);
        //process
        if(sent < 0){
            //stop (IO error)
            PRINTF_ERROR("TNLyrIO, NBIOLnk_write failed with (%d).\n", sent);
            opq->io.write.isBlocked = opq->io.write.isShuttedDown = TRUE;
            break;
        } else {
            if(sent > 0){
                //PRINTF_INFO("TNLyrIO, (%s) NBIOLnk_write sent %d bytes.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), sent);
                /*{
                 STNBString str;
                 NBString_initWithStrBytes(&str, (const char*)&buffs->read->data[buffs->read->csmd], sent);
                 PRINTF_INFO("TNLyrIO, NBIOLnk_write sent %d bytes:--->\n%s<---.\n", sent, str.str);
                 NBString_release(&str);
                 }*/
                TNBuffs_moveCsmCursor(buffs, sent);
            }
            //wait for flag to be cleaned
            opq->io.write.isBlocked = (sent < sendAvail);
        }
        IF_NBASSERT(dbgWritesCount++;)
        IF_NBASSERT(if(dbgWritesCount > 3){ PRINTF_WARNING("TNLyrIO, (%s) %d io-writes.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), dbgWritesCount); })
    }
    //flush
    if(!TNBuffs_canConsume(buffs) && opq->lyr.toMe.fromUp.write.isFlushPend && !opq->io.write.isShuttedDown){
        opq->lyr.toMe.fromUp.write.isFlushPend = FALSE;
        NBIOLnk_flush(ioLnk);
    }
}

void TNLyrIO_pollConsumeMaskOpqLocked_(STTNLyrIOOpq* opq, STNBIOLnk* ioLnk, const UI8 pollMask, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync) {
    NBASSERT(opq->io.read.buffs != NULL && opq->io.write.buffs != NULL) //program logic error, buffs-cfg must be made at all paths before this call.
        //error
        if (pollMask & ENNBIOPollsterOpBits_ErrOrGone) {
            PRINTF_INFO("TNLyrIO, conn-error-or-gone.\n");
            opq->io.read.isShuttedDown = opq->io.write.isShuttedDown = TRUE;
        }
    //unset blocking-flags
    if (pollMask & ENNBIOPollsterOpBit_Read) { //write is implicit available untill explicit unlocked.
        opq->io.read.isBlocked = FALSE;
    }
    if (pollMask & ENNBIOPollsterOpBit_Write) { //write is implicit available untill explicit unlocked.
        opq->io.write.isBlocked = FALSE;
    }
    //handshake timeout
    if (!opq->io.handshake.isCompleted) {
        if (pollMask & (ENNBIOPollsterOpBit_Read | ENNBIOPollsterOpBit_Write)) {
            //IF_PRINTF(const UI64 ms = NBTimestampMicro_getDiffNowInMs(&opq->io.handshake.startTime);)
            //PRINTF_INFO("TNLyrIO, handshake completed after %llu.%llu secs.\n", (ms / 1000), ((ms % 1000) / 100));
            opq->io.handshake.isCompleted = TRUE;
        } else {
            const UI64 ms = NBTimestampMicro_getDiffNowInMs(&opq->io.handshake.startTime);
            if (ms > (5 * 1000) || NBStopFlag_isAnyActivated(opq->stopFlag) || (opq->io.read.isShuttedDown && opq->io.write.isShuttedDown) || (opq->lyr.fromMe.toLwr.read.isShuttedDown && opq->lyr.fromMe.toLwr.write.isShuttedDown)) {
                PRINTF_ERROR("TNLyrIO, handshake timedout after %llu.%llu secs.\n", (ms / 1000), ((ms % 1000) / 100));
                opq->io.read.isShuttedDown = TRUE;
                opq->io.write.isShuttedDown = TRUE;
            }
        }
    }
    //cycle
    if (opq->io.handshake.isCompleted && ioLnk != NULL) {
        //read
        TNLyrIO_pollReadOpq_(opq, ioLnk);
        //write
        TNLyrIO_pollWriteOpq_(opq, ioLnk);
    }
    //apply stopFlag (inmediatly)
    if (NBStopFlag_isAnyActivated(opq->stopFlag)) {
        opq->io.read.isShuttedDown = opq->io.write.isShuttedDown = TRUE;
    }
    //sync
    if (opq->flow == ENTNLyrFlow_FromLwr) {
        //apply upper/lwr shutdowns to me (after consumeMask)
        //note: in outgoing flow only 'toMe.fromLwr' applies
        {
            if (!opq->io.write.isShuttedDown && ((opq->lyr.toMe.fromLwr.read.isShuttedDown || opq->lyr.fromMe.toLwr.read.isShuttedDown) && !TNBuffs_canConsume(opq->io.write.buffs) /*can flush*/)) {
                //PRINTF_INFO("TNLyrIO, (%s) shutting write after lwr layer's.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
                opq->io.write.isShuttedDown = TRUE;
                if (ioLnk != NULL) {
                    NBIOLnk_shutdown(ioLnk, NB_IO_BIT_WRITE);
                }
            }
            if (!opq->io.read.isShuttedDown && (opq->lyr.toMe.fromLwr.write.isShuttedDown || opq->lyr.fromMe.toLwr.write.isShuttedDown) /*inmediate*/) {
                //PRINTF_INFO("TNLyrIO, (%s) shutting read after lwr layer's.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
                opq->io.read.isShuttedDown = TRUE;
                if (ioLnk != NULL) {
                    NBIOLnk_shutdown(ioLnk, NB_IO_BIT_READ);
                }
            }
        }
    } else if (opq->flow == ENTNLyrFlow_FromUp) {
        //push to nxt-lyr
        if (opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask != NULL) {
            NBASSERT(!opq->lyr.toMe.lnkIsEnabled)
                opq->lyr.toMe.lnkIsEnabled = TRUE;
            {
                UI8 pollReqNxt = 0;
                const UI8 pollMaskNxt = (TNBuffs_canConsume(&opq->io.buffs.toUpLyr) ? ENNBIOPollsterOpBit_Read : 0) | (TNBuffs_canFill(&opq->io.buffs.fromUpLyr) ? ENNBIOPollsterOpBit_Write : 0);
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrConsumeMask)(&opq->lyr.toMe.lnk, pollMaskNxt, &pollReqNxt, opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
            opq->lyr.toMe.lnkIsEnabled = FALSE;
        }
        //apply upper/lwr shutdowns to me (after consumeMask)
        //note: in incoming flow only 'toMe.fromUp' applies
        {
            if (!opq->io.read.isShuttedDown && opq->lyr.toMe.fromUp.read.isShuttedDown /*inmediate*/) {
                //shutdown my read
                //PRINTF_INFO("TNLyrIO, (%s) shutting read after upper layer's.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
                opq->io.read.isShuttedDown = TRUE;
                if (ioLnk != NULL) {
                    NBIOLnk_shutdown(ioLnk, NB_IO_BIT_READ);
                }
            }
            //
            if (!opq->io.write.isShuttedDown && (opq->lyr.toMe.fromUp.write.isShuttedDown && !TNBuffs_canConsume(opq->io.write.buffs) /*can flush*/)) {
                //shutdown my write
                //PRINTF_INFO("TNLyrIO, (%s) shutting write after upper layer's.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
                opq->io.write.isShuttedDown = TRUE;
                if (ioLnk != NULL) {
                    NBIOLnk_shutdown(ioLnk, NB_IO_BIT_WRITE);
                }
            }
        }
        //notify my shutdowns to upLyr and lwrLyr (after consumeMask)
        {
            UI8 shutMaskUp = 0;
            //
            if (!opq->io.read.isShutNotified && opq->io.read.isShuttedDown) {
                opq->io.read.isShutNotified = TRUE;
                if (!opq->lyr.toMe.fromUp.read.isShuttedDown) { //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_READ; //you cannot read from this lyr anymore
                }
            }
            //
            if (!opq->io.write.isShutNotified && opq->io.write.isShuttedDown) {
                opq->io.write.isShutNotified = TRUE;
                if (!opq->lyr.toMe.fromUp.write.isShuttedDown) { //no need to notify if upper layer already shutted down
                    shutMaskUp |= NB_IO_BIT_WRITE; //you cannot write to this lyr anymore
                }
            }
            IF_PRINTF(
                if (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL) {
                    //PRINTF_INFO("TNLyrIO, (%s) notifying %s%s%s%s.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? "upLyr-shut(" : ""), (shutMaskUp & NB_IO_BIT_READ ? "rd" : ""), (shutMaskUp & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL ? ")" : ""));
                }
            )
                if (shutMaskUp != 0 && opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown != NULL) {
                    (*opq->lyr.fromMe.toUp.lstnr.itf.lyrShutdown)(shutMaskUp, opq->lyr.fromMe.toUp.lstnr.usrParam);
                }
        }
    }
    //return
    //stops reading after stopFlag is activated.
    //continues writting after stopFlag was activated (to flush if posible).
    if (dstUpd != NULL) {
        dstUpd->opsMasks = TNLyrIO_ioGetPollMask(opq);
        //dbg
        /*if(opq->io.handshake.isCompleted && dstUpd->opsMasks != 0 && opq->flow == ENTNLyrFlow_FromUp){
            PRINTF_INFO("TNLyrIO, (%s) %s%s%s.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), (dstUpd->opsMasks & ENNBIOPollsterOpBit_Read ? "canRcv" : ""), (dstUpd->opsMasks == (ENNBIOPollsterOpBit_Read | ENNBIOPollsterOpBit_Write) ? ", " : ""), (dstUpd->opsMasks & ENNBIOPollsterOpBit_Write ? "canSend" : ""))
        }*/
    }
    //consume stopFlag (only if no io-actions are expected and upper layers are not bussy, this allows to flush data)
    if (!TNLyrIO_imBussy(opq) && (opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning == NULL || !(*opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning)(opq->lyr.fromMe.toUp.lstnr.usrParam))) {
        if (ioLnk != NULL) {
            //close
            NBIOLnk_close(ioLnk);
            //remove from pollster
            if (NBIOPollsterSync_isSet(dstSync)) {
                NBASSERT(opq->pollster.isListening)
                    NBIOPollsterSync_removeIOLnk(dstSync, ioLnk);
                PRINTF_INFO("TNLyrIO, (%s) sync-removing from pollster.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
            }
        }
        //notify up-lyrs
        if (opq->flow == ENTNLyrFlow_FromUp) {
            //up-lyr
            if (opq->lyr.fromMe.toUp.lstnr.itf.lyrClose != NULL) {
                (*opq->lyr.fromMe.toUp.lstnr.itf.lyrClose)(opq->lyr.fromMe.toUp.lstnr.usrParam);
            }
        }
    }
}

void TNLyrIO_pollConsumeMask_(STNBIOLnk ioLnk, const UI8 pollMask, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData;
    IF_NBASSERT(STNBTimestampMicro timeLastAction = NBTimestampMicro_getUTC(), timeCur; SI64 usDiff;)
    NBObject_lock(opq);
    {
        TNLyrIO_pollConsumeMaskOpqLocked_(opq, &ioLnk, pollMask, dstUpd, dstSync);
    }
    //print-tick-time
#   ifdef NB_CONFIG_INCLUDE_ASSERTS
    {
        timeCur   = NBTimestampMicro_getUTC();
        usDiff    = NBTimestampMicro_getDiffInUs(&timeLastAction, &timeCur);
        /*if(usDiff >= 1000ULL){
         PRINTF_INFO("TNLyrIO, pollConsumeMask(%s%s%s) took %llu.%llu%llums.\n", (pollMask & ENNBIOPollsterOpBit_Read ? "+read" : ""), (pollMask & ENNBIOPollsterOpBit_Write ? "+write" : ""), (pollMask & ENNBIOPollsterOpBits_ErrOrGone ? "+errOrGone" : ""), (usDiff / 1000ULL), (usDiff % 1000ULL) % 100ULL, (usDiff % 100ULL) % 10ULL);
         }*/
        timeLastAction = timeCur;
    }
#   endif
    NBObject_unlock(opq);
}

void TNLyrIO_pollConsumeNoOp_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData){ //consume non-ops and return the ones that are required to poll.
    TNLyrIO_pollConsumeMask_(ioLnk, 0, dstUpd, dstSync, usrData);
}

void TNLyrIO_pollGetReqUpd_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, void* usrData){ //optional, if the client expects to change the requested 'opsMasks' outside the 'pollConsumeMask' or 'pollConsumeNoOp' calls.
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        NBASSERT(opq->pollster.isListening)
        //stops reading after stopFlag is activated.
        //continues writting after stopFlag was activated (to flush if posible).
        if(dstUpd != NULL){
            dstUpd->opsMasks = TNLyrIO_ioGetPollMask(opq);
            //dbg
            /*if(opq->io.handshake.isCompleted && dstUpd->opsMasks != 0){
                PRINTF_INFO("TNLyrIO, (%s) %s%s%s.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), (dstUpd->opsMasks & ENNBIOPollsterOpBit_Read ? "canRcv" : ""), (dstUpd->opsMasks == (ENNBIOPollsterOpBit_Read | ENNBIOPollsterOpBit_Write) ? ", " : ""), (dstUpd->opsMasks & ENNBIOPollsterOpBit_Write ? "canSend" : ""))
            }*/
        }
    }
    NBObject_unlock(opq);
}

void TNLyrIO_pollRemoved_(STNBIOLnk ioLnk, void* usrData){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        NBASSERT(opq->pollster.isListening)
        opq->pollster.isListening = FALSE;
        NBThreadCond_broadcast(&opq->cond);
        //
        PRINTF_INFO("TNLyrIO, %s io removed from pollster.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
        NBStopFlag_activate(opq->stopFlag);
    }
    NBObject_unlock(opq);
}

//lyr chain

BOOL TNLyrIO_lyrSetNext_(const STTNLyrLstnr* nxt, void* usrData){ //set next lyr iterface
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
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
        if(opq->flow == ENTNLyrFlow_FromLwr){
            PRINTF_WARNING("TNLyrIO, outgoing-io-lyr cant have a nxt-lyr.\n");
        } else {
            //apply
            toRelease = opq->lyr.fromMe.toUp.lstnr;
            opq->lyr.fromMe.toUp.lstnr = *nxt;
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

void TNLyrIO_lyrStart_(void* usrData){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    if(opq->isPrepared && !opq->isStarted){
        //upper
        if(opq->lyr.fromMe.toUp.lstnr.itf.lyrStart != NULL){
            (*opq->lyr.fromMe.toUp.lstnr.itf.lyrStart)(opq->lyr.fromMe.toUp.lstnr.usrParam);
        }
        //add to pollster
        if(!opq->pollster.isListening && NBIOPollsterSync_isSet(opq->pollster.sync)){
            STNBIOPollsterLstrnItf itf;
            NBMemory_setZeroSt(itf, STNBIOPollsterLstrnItf);
            itf.pollConsumeMask = TNLyrIO_pollConsumeMask_;
            itf.pollConsumeNoOp = TNLyrIO_pollConsumeNoOp_;
            itf.pollRemoved     = TNLyrIO_pollRemoved_;
            //Notes:
            //In 'ENTNLyrFlow_FromUp' mode, the 'pollGetReqUpd' is not required.
            //   On the same testing env: no bandwidth reduction was visible by ignoring the 'pollGetReqUpd' callback.
            //Inn 'ENTNLyrFlow_FromLwr' mode, the 'pollGetReqUpd' is sesntial to acoid io-bandwidth loss.
            //   On the same testing env: ~300Mbps is reduced to ~10Mbps if this callback is missing.
            if(opq->flow == ENTNLyrFlow_FromLwr){
                itf.pollGetReqUpd = TNLyrIO_pollGetReqUpd_; //optional, if the client expects to change the requested 'opsMasks' outside the 'pollConsumeMask' or 'pollConsumeNoOp' calls.
            }
            //
            if(NBFile_isSet(opq->io.file)){
                opq->pollster.isListening = TRUE; //set flag first in case callback is called
                if(!NBIOPollsterSync_addFileWithItf(opq->pollster.sync, opq->io.file, ENNBIOPollsterOpBit_None, &itf, opq)){
                    PRINTF_ERROR("TNLyrIO, NBIOPollsterSync_addFileWithItf failed.\n");
                    opq->pollster.isListening = FALSE;
                } else {
                    NBThreadCond_broadcast(&opq->cond);
                }
            } else if(NBSocket_isSet(opq->io.socket)){
                opq->pollster.isListening = TRUE; //set flag first in case callback is called
                if(!NBIOPollsterSync_addSocketWithItf(opq->pollster.sync, opq->io.socket, ENNBIOPollsterOpBit_None, &itf, opq)){
                    PRINTF_ERROR("TNLyrIO, NBIOPollsterSync_addSocketWithItf failed.\n");
                    opq->pollster.isListening = FALSE;
                } else {
                    NBThreadCond_broadcast(&opq->cond);
                }
            }
        }
        //
        opq->isStarted = TRUE;
    }
    NBObject_unlock(opq);
}

BOOL TNLyrIO_lyrIsRunning_(void* usrData){    //cleanup must wait
    BOOL r = FALSE;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        //flushing write data or upper layer isBussy
        r = (
             //my flags
             opq->pollster.isListening || TNLyrIO_imBussy(opq)
             //upper flags
             || (opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning != NULL && (*opq->lyr.fromMe.toUp.lstnr.itf.lyrIsRunning)(opq->lyr.fromMe.toUp.lstnr.usrParam))
            );
    }
    NBObject_unlock(opq);
    return r;
}

void TNLyrIO_lyrConsumeMask_(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        UI8 pollReqNxt = 0;
        //
        //if not self-running, tick as connection-gone to trigger/continue the cleanup
        if (!opq->pollster.isListening) {
            TNLyrIO_pollConsumeMaskOpqLocked_(opq, NULL, ENNBIOPollsterOpBits_ErrOrGone, NULL, NB_OBJREF_NULL);
        }
        //
        //error
        if (pollMask & ENNBIOPollsterOpBits_ErrOrGone) {
            PRINTF_INFO("TNLyrIO, lyr-error-or-gone.\n");
            opq->lyr.fromMe.toLwr.read.isShuttedDown = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
        }
        if (pollMask & ENNBIOPollsterOpBit_Read) {
            opq->lyr.fromMe.toLwr.read.isBlocked = FALSE;
        }
        if (pollMask & ENNBIOPollsterOpBit_Write) {
            opq->lyr.fromMe.toLwr.write.isBlocked = FALSE;
        }
        //
        if(opq->flow == ENTNLyrFlow_FromLwr){
            //push to prev-lyr
            //read
            if(lnk != NULL && lnk->itf.ioRead != NULL){
                IF_NBASSERT(UI32 dbgReadsCount = 0;)
                STTNBuffs* buffs = &opq->io.buffs.toUpLyr;
                while(TNBuffs_canFill(buffs) && !opq->lyr.fromMe.toLwr.read.isBlocked && !opq->lyr.fromMe.toLwr.read.isShuttedDown){
                    void* dst           = &buffs->fill->data[buffs->fill->filled];
                    const UI32 dstSz    = buffs->fill->size - buffs->fill->filled;
                    const SI32 rr       = (*lnk->itf.ioRead)(dst, dstSz, lnk->usrData);
                    if(rr < 0){
                        PRINTF_ERROR("TNLyrIO, (%s) ioRead failed from down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                        opq->lyr.fromMe.toLwr.read.isBlocked = opq->lyr.fromMe.toLwr.read.isShuttedDown = TRUE;
                        break;
                    } else {
                        if(rr > 0){
                            //PRINTF_INFO("TNLyrIO, (%s) ioRead from down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                            TNBuffs_moveFillCursor(buffs, rr);
                        }
                        opq->lyr.fromMe.toLwr.read.isBlocked = (rr < dstSz);
                    }
                    IF_NBASSERT(dbgReadsCount++;)
                    IF_NBASSERT(if(dbgReadsCount > 3){ PRINTF_WARNING("TNLyrIO, (%s) %d reads.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), dbgReadsCount); })
                }
                //next-poll-mask
                pollReqNxt |= (TNBuffs_canFill(buffs) ? ENNBIOPollsterOpBit_Read : 0);
            }
            //write
            if(lnk != NULL && lnk->itf.ioWrite != NULL){
                IF_NBASSERT(UI32 dbgWritesCount = 0;)
                STTNBuffs* buffs = &opq->io.buffs.fromUpLyr;
                while(TNBuffs_canConsume(buffs) && !opq->lyr.fromMe.toLwr.write.isBlocked && !opq->lyr.fromMe.toLwr.write.isShuttedDown){
                    const void* src     = &buffs->read->data[buffs->read->csmd];
                    const UI32 srcSz    = buffs->read->filled - buffs->read->csmd;
                    const SI32 rr       = (*lnk->itf.ioWrite)(src, srcSz, lnk->usrData);
                    if(rr < 0){
                        PRINTF_ERROR("TNLyrIO, ioWrite failed down-lyr (%d).\n", rr);
                        opq->lyr.fromMe.toLwr.write.isBlocked = opq->lyr.fromMe.toLwr.write.isShuttedDown = TRUE;
                        break;
                    } else {
                        if(rr > 0){
                            //PRINTF_INFO("TNLyrIO, (%s) ioWrite to down-lyr (%d).\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), rr);
                            TNBuffs_moveCsmCursor(buffs, rr);
                        }
                        opq->lyr.fromMe.toLwr.write.isBlocked = (rr < srcSz);
                    }
                    IF_NBASSERT(dbgWritesCount++;)
                    IF_NBASSERT(if(dbgWritesCount > 3){ PRINTF_WARNING("TNLyrIO, (%s) %d writes.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), dbgWritesCount); })
                }
                //next-poll-mask
                pollReqNxt |= (TNBuffs_canConsume(buffs) ? ENNBIOPollsterOpBit_Write : 0);
            }
            //notify my shutdowns to upLyr and lwrLyr (after consumeMask)
            {
                UI8 shutMaskLwr = 0;
                //
                if(!opq->io.read.isShutNotified && opq->io.read.isShuttedDown){
                    opq->io.read.isShutNotified = TRUE;
                    if(!opq->lyr.toMe.fromLwr.write.isShuttedDown){
                        shutMaskLwr |= NB_IO_BIT_WRITE;
                    }
                }
                //
                if(!opq->io.write.isShutNotified && opq->io.write.isShuttedDown){
                    opq->io.write.isShutNotified = TRUE;
                    if(!opq->lyr.toMe.fromLwr.read.isShuttedDown){
                        shutMaskLwr |= NB_IO_BIT_READ;
                    }
                }
                IF_PRINTF(
                            if((shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL)){
                                //PRINTF_INFO("TNLyrIO, (%s) notifying %s%s%s%s.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"), (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? "lwrLyr-shut(" : ""), (shutMaskLwr & NB_IO_BIT_READ ? "rd" : ""), (shutMaskLwr & NB_IO_BIT_WRITE ? "wr" : ""), (shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL ? ")" : ""));
                            }
                )
                //incoming flows do not have a lwrLyr
                if(shutMaskLwr != 0 && lnk->itf.ioShutdown != NULL){
                    (*lnk->itf.ioShutdown)(shutMaskLwr, lnk->usrData);
                }
            }
        }
        //
        if(dstPollReq != NULL){
            *dstPollReq = pollReqNxt;
        }
        //notify stopFlag to lower-layer(s)
        if(NBStopFlag_isMineActivated(opq->stopFlag)){
            if(lnk != NULL && lnk->itf.ioClose != NULL){
                (*lnk->itf.ioClose)(lnk->usrData);
            }
        }
    }
    NBObject_unlock(opq);
}

void TNLyrIO_lyrShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrIO_lyrClose_(void* usrData){
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    NBObject_lock(opq);
    {
        opq->lyr.toMe.fromLwr.read.isShuttedDown = opq->lyr.toMe.fromLwr.write.isShuttedDown = TRUE;
    }
    NBObject_unlock(opq);
}

void TNLyrIO_lyrConcat_(STNBString* dst, void* usrData) {
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    if (dst != NULL) {
        NBObject_lock(opq);
        {
            NBString_concat(dst, "TNLyrIO");
            NBString_concat(dst, (NBSocket_isSet(opq->io.socket) ? "s" : NBFile_isSet(opq->io.file) ? "f" : "?"));
            NBString_concat(dst, " (");
            NBString_concat(dst, (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
            NBString_concat(dst, ") r("); NBString_concat(dst, opq->lyr.fromMe.toLwr.read.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ") w("); NBString_concat(dst, opq->lyr.fromMe.toLwr.write.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ") ioR("); NBString_concat(dst, opq->io.read.isShuttedDown ? "shut" : "actv");
            NBString_concat(dst, ") ioW("); NBString_concat(dst, opq->io.write.isShuttedDown ? "shut" : "actv");
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

SI32 TNLyrIO_ioRead_(void* pDst, const SI32 dstSz, void* usrData) { //read data to destination buffer, returns the ammount of bytes read, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.read.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be reading anymore.
        } else if(dstSz == 0){
            r = 0;
        } else if(dstSz > 0){
            STTNBuffs* buffs = &opq->io.buffs.toUpLyr;
            r = TNBuffs_consume(buffs, pDst, dstSz);
            if(r == 0 && TNLyrIO_ioBuffsIsShuttedDown(opq, buffs)){
                PRINTF_INFO("TNLyrIO, (%s) ioRead sending EOF-sginal.\n", (opq->flow == ENTNLyrFlow_FromUp ? "in" : opq->flow == ENTNLyrFlow_FromLwr ? "out" : "???"));
                r = NB_IO_ERR_EOF; //buffer is empty and no new data is expected
            }
        }
    }
    return r;
}

SI32 TNLyrIO_ioWrite_(const void* pSrc, const SI32 srcSz, void* usrData){ //write data from source buffer, returns the ammount of bytes written, negative in case of error
    SI32 r = NB_IO_ERROR;
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        if(opq->lyr.toMe.fromUp.write.isShuttedDown){
            r = NB_IO_ERR_SHUTTED_DOWN; //you told me wont be writting anymore.
        } else if(srcSz == 0){
            r = 0;
        } else if(srcSz > 0){
            STTNBuffs* buffs = &opq->io.buffs.fromUpLyr;
            if(TNLyrIO_ioBuffsIsShuttedDown(opq, buffs)){
                r = NB_IO_ERROR; //io wont be consuming write buffer anymore
            } else {
                if(opq->lyr.toMe.fromUp.write.isFlushPend){
                    //do not fill write buffer untill flushed.
                    r = 0;
                } else {
                    r = TNBuffs_fill(buffs, pSrc, srcSz);
                }
            }
        }
    }
    return r;
}

void TNLyrIO_ioFlush_(void* usrData){ //flush write-data
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        NBASSERT(opq->flow == ENTNLyrFlow_FromUp) //must be incoming-io
        opq->lyr.toMe.fromUp.write.isFlushPend = TRUE;
    }
}

void TNLyrIO_ioShutdown_(const UI8 mask, void* usrData){ //NB_IO_BIT_READ | NB_IO_BIT_WRITE
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
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

void TNLyrIO_ioClose_(void* usrData){ //close ungracefully
    STTNLyrIOOpq* opq = (STTNLyrIOOpq*)usrData; NBASSERT(TNLyrIO_isClass(NBObjRef_fromOpqPtr(opq)))
    //this call occurs while opq is locked
    NBASSERT(opq->lyr.toMe.lnkIsEnabled) //this should be called only inside 'nxt.consumeMask' call
    if(opq->lyr.toMe.lnkIsEnabled){
        opq->lyr.toMe.fromUp.read.isShuttedDown = opq->lyr.toMe.fromUp.write.isShuttedDown = TRUE;
    }
}
