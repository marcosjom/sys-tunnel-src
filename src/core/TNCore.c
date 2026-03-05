//
//  TNCore.c
//  sys-tunnel-core-osx
//
//  Created by Marcos Ortega on 15/3/23.
//

#include "nb/NBFrameworkPch.h"
#include "nb/NBObject.h"
#include "nb/core/NBMemory.h"
#include "nb/core/NBNumParser.h"
#include "nb/core/NBStruct.h"
#include "nb/core/NBMngrStructMaps.h"
#include "nb/core/NBArray.h"
#include "nb/crypto/NBX509.h"
#include "nb/crypto/NBBase64.h"
//
#include "core/TNCore.h"
#include "core/TNCorePort.h"
#include "core/TNLyrIO.h"
#include "core/TNLyrMask.h"
#include "core/TNLyrBase64.h"
#include "core/TNLyrDump.h"

//TNCoreArgs

STNBStructMapsRec TNCoreArgs_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreArgs_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreArgs_sharedStructMap);
    if(TNCoreArgs_sharedStructMap.map == NULL){
        STTNCoreArgs s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreArgs);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addStructM(map, s, cfg, TNCoreCfg_getSharedStructMap());
        NBStructMap_addUIntM(map, s, secsRunAndExit);       //secs to run and auto-stop
        NBStructMap_addUIntM(map, s, maxSecsWithoutConn);   //max secs to allow without first/next client conn (auto-stops)
        NBStructMap_addUIntM(map, s, maxConnsAndExit);      //max conns to receive (stops receiving after) and exits after last conn leaves.
        NBStructMap_addBoolM(map, s, printArgs);            //prints parsed/loaded args
        NBStructMap_addBoolM(map, s, printCfg);             //prints parsed/loaded args
        NBStructMap_addBoolM(map, s, printHelp);            //prints help
        TNCoreArgs_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreArgs_sharedStructMap);
    return TNCoreArgs_sharedStructMap.map;
}


//TNCoreOpq

typedef struct STTNCoreOpq_ {
    STNBObject          prnt;
    STNBStopFlagRef     stopFlag;
    STTNCoreArgs        args;
    BOOL                isPrepared;
    BOOL                isListening;
    BOOL                isRunning;
    //pollster
    struct {
        STNBIOPollsterRef       def;
        STNBIOPollsterSyncRef   sync; //default
        STNBIOPollstersProviderRef provider; //provider
    } pollster;
    //ports
    struct {
        STNBArray   arr;        //STTNCorePortRef
    } ports;
    //conns
    struct {
        STNBArray   arr;                    //STTNLyrLstnr, btm lyr of each stack (in-socket)
        STNBTimestampMicro lastAddTime;     //arrival of connection
        STNBTimestampMicro lastRemoveTime;  //cleanup of connection
        UI32        totalArrived;           //total conns arrived (even if initialization failed)
    } conns;
    //CAs (default global list)
    struct {
        STNBArray   arr;        //STNBX509
    } CAs;
} STTNCoreOpq;

//
BOOL TNCore_prepareLyrsInvOrderOpq_(STTNCoreOpq* opq, const STTNCoreCfgPort* cfg, STNBArray* dst /*STTNLyrLstnr*/);
BOOL TNCore_portConnArrivedOpq_(STTNCorePortRef ref, STNBSocketRef socket, const STTNCoreCfgPort* cfg, STNBSslContextRef sslCtx, const STNBX509* sslCAs, const UI32 sslCAsSz, STNBSslContextRef redirSslCtx, const STNBX509* redirSslCAs, const UI32 redirSslCAsSz, void* usrParam);

//

NB_OBJREF_BODY(TNCore, STTNCoreOpq, NBObject)

void TNCore_initZeroed(STNBObject* obj){
    STTNCoreOpq* opq    = (STTNCoreOpq*)obj;
    opq->stopFlag       = NBStopFlag_alloc(NULL);
    //ports
    {
        NBArray_init(&opq->ports.arr, sizeof(STTNCorePortRef), NULL);
    }
    //conns
    {
        NBArray_init(&opq->conns.arr, sizeof(STTNLyrLstnr), NULL);
        opq->conns.lastAddTime = opq->conns.lastRemoveTime = NBTimestampMicro_getUTC();
    }
    //CAs
    {
        NBArray_init(&opq->CAs.arr, sizeof(STNBX509), NULL);
    }
}

void TNCore_uninitLocked(STNBObject* obj){
    STTNCoreOpq* opq = (STTNCoreOpq*)obj;
    //
    if(NBStopFlag_isSet(opq->stopFlag)){
        NBStopFlag_activate(opq->stopFlag);
    }
    //pollster
    {
        if(NBIOPollster_isSet(opq->pollster.def)){
            NBIOPollster_release(&opq->pollster.def);
            NBIOPollster_null(&opq->pollster.def);
        }
        if(NBIOPollsterSync_isSet(opq->pollster.sync)){
            NBIOPollsterSync_release(&opq->pollster.sync);
            NBIOPollsterSync_null(&opq->pollster.sync);
        }
        if(NBIOPollstersProvider_isSet(opq->pollster.provider)){
            NBIOPollstersProvider_release(&opq->pollster.provider);
            NBIOPollstersProvider_null(&opq->pollster.provider);
        }
    }
    //ports
    {
        SI32 i; for(i = 0; i < opq->ports.arr.use; i++){
            STTNCorePortRef p = NBArray_itmValueAtIndex(&opq->ports.arr, STTNCorePortRef, i);
            if(TNCorePort_isSet(p)){
                TNCorePort_release(&p); //internally will be stopped
                TNCorePort_null(&p);
            }
        }
        NBArray_empty(&opq->ports.arr);
        NBArray_release(&opq->ports.arr);
    }
    //conns
    {
        SI32 i; for(i = 0; i < opq->conns.arr.use; i++){
            STTNLyrLstnr s = NBArray_itmValueAtIndex(&opq->conns.arr, STTNLyrLstnr, i);
            if(s.itf.lyrRelease != NULL){
                (*s.itf.lyrRelease)(s.usrParam);
            }
        }
        NBArray_empty(&opq->conns.arr);
        NBArray_release(&opq->conns.arr);
    }
    //CAs
    {
        SI32 i; for(i = 0; i < opq->CAs.arr.use; i++){
            STNBX509* c = NBArray_itmPtrAtIndex(&opq->CAs.arr, STNBX509, i);
            NBX509_release(c);
        }
        NBArray_empty(&opq->CAs.arr);
        NBArray_release(&opq->CAs.arr);
    }
    //
    opq->isPrepared   = FALSE;
    opq->isListening  = FALSE;
    //
    if(NBStopFlag_isSet(opq->stopFlag)){
        NBStopFlag_release(&opq->stopFlag);
        NBStopFlag_null(&opq->stopFlag);
    }
}

BOOL TNCore_parseArgsCfg_(const int argc, const char* argv[], STTNCoreCfg* dst, STNBString* dstErrLst){
    BOOL r = FALSE;
    if(dst != NULL && argv != NULL){
        //const UI32 casSzBefore = dst->casSz;
        STNBString path, errsLst;
        NBString_init(&path);
        NBString_init(&errsLst);
        {
            int i; for(i = 0; i < argc && errsLst.length == 0; i++){
                const char* arg = argv[i];
                if(NBString_strIsLike(arg, "-CAs")){
                    //-CAs
                    if(NBString_startsWith(&path, "/port/ssl")){
                        NBString_set(&path, "/port/ssl/CAs");
                    } else if(NBString_startsWith(&path, "/port/redir/ssl")){
                        NBString_set(&path, "/port/redir/ssl/CAs");
                    } else {
                        NBString_set(&path, "/CAs");
                    }
                } else if(NBString_strIsLike(arg, "-path")){
                    //-path
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        if(NBString_startsWith(&path, "/CAs")){
                            STTNCoreCfgSslCertSrc* rec = NULL;
                            STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, dst->casSz + 1);
                            if(dst->cas != NULL){ int i2; for(i2 = 0; i2 < dst->casSz; i2++){ nArr[i2] = dst->cas[i2]; } NBMemory_free(dst->cas); }
                            dst->cas = nArr;
                            rec = &dst->cas[dst->casSz]; dst->casSz++;
                            NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                            rec->path = NBString_strNewBuffer(argv[i + 1]); i++;
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.key.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.key.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/redir/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->redir.ssl.cert.source.key.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/redir/ssl/cert/source")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->redir.ssl.cert.source.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/CAs")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                STTNCoreCfgSslCertSrc* rec = NULL;
                                STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, p->ssl.casSz + 1);
                                if(p->ssl.cas != NULL){ int i2; for(i2 = 0; i2 < p->ssl.casSz; i2++){ nArr[i2] = p->ssl.cas[i2]; } NBMemory_free(p->ssl.cas); }
                                p->ssl.cas = nArr;
                                rec = &p->ssl.cas[p->ssl.casSz]; p->ssl.casSz++;
                                NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                                rec->path = NBString_strNewBuffer(argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.key.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/cert/source")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.path, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/CAs")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                STTNCoreCfgSslCertSrc* rec = NULL;
                                STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, p->redir.ssl.casSz + 1);
                                if(p->redir.ssl.cas != NULL){ int i2; for(i2 = 0; i2 < p->redir.ssl.casSz; i2++){ nArr[i2] = p->redir.ssl.cas[i2]; } NBMemory_free(p->redir.ssl.cas); }
                                p->redir.ssl.cas = nArr;
                                rec = &p->redir.ssl.cas[p->redir.ssl.casSz]; p->redir.ssl.casSz++;
                                NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                                rec->path = NBString_strNewBuffer(argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '-path' param.");
                        }
                    }
                } else if(NBString_strIsLike(arg, "-pay64")){
                    //-pay64
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        if(NBString_startsWith(&path, "/CAs")){
                            STTNCoreCfgSslCertSrc* rec = NULL;
                            STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, dst->casSz + 1);
                            if(dst->cas != NULL){ int i2; for(i2 = 0; i2 < dst->casSz; i2++){ nArr[i2] = dst->cas[i2]; } NBMemory_free(dst->cas); }
                            dst->cas = nArr;
                            rec = &dst->cas[dst->casSz]; dst->casSz++;
                            NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                            rec->pay64 = NBString_strNewBuffer(argv[i + 1]); i++;
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.key.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.key.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/CAs")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                STTNCoreCfgSslCertSrc* rec = NULL;
                                STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, p->ssl.casSz + 1);
                                if(p->ssl.cas != NULL){ int i2; for(i2 = 0; i2 < p->ssl.casSz; i2++){ nArr[i2] = p->ssl.cas[i2]; } NBMemory_free(p->ssl.cas); }
                                p->ssl.cas = nArr;
                                rec = &p->ssl.cas[p->ssl.casSz]; p->ssl.casSz++;
                                NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                                rec->pay64 = NBString_strNewBuffer(argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.key.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/cert/source")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.pay64, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/redir/ssl/CAs")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                STTNCoreCfgSslCertSrc* rec = NULL;
                                STTNCoreCfgSslCertSrc* nArr = NBMemory_allocTypes(STTNCoreCfgSslCertSrc, p->redir.ssl.casSz + 1);
                                if(p->redir.ssl.cas != NULL){ int i2; for(i2 = 0; i2 < p->redir.ssl.casSz; i2++){ nArr[i2] = p->redir.ssl.cas[i2]; } NBMemory_free(p->redir.ssl.cas); }
                                p->redir.ssl.cas = nArr;
                                rec = &p->redir.ssl.cas[p->redir.ssl.casSz]; p->redir.ssl.casSz++;
                                NBMemory_setZeroSt(*rec, STTNCoreCfgSslCertSrc);
                                rec->pay64 = NBString_strNewBuffer(argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '-path' param.");
                        }
                    }
                } else if(NBString_strIsLike(arg, "-port")){
                    //-port
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            if(NBString_startsWith(&path, "/port/redir")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    if(p->redir.port == 0){
                                        p->redir.port = v;
                                    } else {
                                        //Add new port
                                        STTNCoreCfgPort* rec = NULL;
                                        STTNCoreCfgPort* nArr = NBMemory_allocTypes(STTNCoreCfgPort, dst->portsSz + 1);
                                        if(dst->ports != NULL){ int i2; for(i2 = 0; i2 < dst->portsSz; i2++){ nArr[i2] = dst->ports[i2]; } NBMemory_free(dst->ports); }
                                        dst->ports = nArr;
                                        rec = &dst->ports[dst->portsSz]; dst->portsSz++;
                                        NBMemory_setZeroSt(*rec, STTNCoreCfgPort);
                                        rec->port = v;
                                        NBString_set(&path, "/port");
                                    }
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else {
                                //Add new port
                                STTNCoreCfgPort* rec = NULL;
                                STTNCoreCfgPort* nArr = NBMemory_allocTypes(STTNCoreCfgPort, dst->portsSz + 1);
                                if(dst->ports != NULL){ int i2; for(i2 = 0; i2 < dst->portsSz; i2++){ nArr[i2] = dst->ports[i2]; } NBMemory_free(dst->ports); }
                                dst->ports = nArr;
                                rec = &dst->ports[dst->portsSz]; dst->portsSz++;
                                NBMemory_setZeroSt(*rec, STTNCoreCfgPort);
                                rec->port = v;
                                NBString_set(&path, "/port");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-io")){
                    if(dst->io == NULL){
                        dst->io = NBMemory_allocType(STTNCoreCfgPort);
                        NBMemory_setZeroSt(*dst->io, STTNCoreCfgPort);
                    }
                    NBString_set(&path, "/io");
                } else if(NBString_strIsLike(arg, "-seed")){
                    //-seed
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            if(NBString_startsWith(&path, "/port/redir/mask")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    p->redir.mask.seed = v;
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/port/mask")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    p->mask.seed = v;
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/io/redir/mask")){
                                if(dst->io != NULL){
                                    dst->io->redir.mask.seed = v;
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else if(NBString_startsWith(&path, "/io/mask")){
                                if(dst->io != NULL){
                                    dst->io->mask.seed = v;
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-pathPrefix")){
                    //-pathPrefix
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        if(vStr[0] == '\0'){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is empty.");
                        } else {
                            if(NBString_startsWith(&path, "/port/redir/dump")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    NBString_strFreeAndNewBuffer(&p->redir.dump.pathPrefix, vStr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/port/dump")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    NBString_strFreeAndNewBuffer(&p->dump.pathPrefix, vStr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/io/redir/dump")){
                                if(dst->io != NULL){
                                    NBString_strFreeAndNewBuffer(&dst->io->redir.dump.pathPrefix, vStr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else if(NBString_startsWith(&path, "/io/dump")){
                                if(dst->io != NULL){
                                    NBString_strFreeAndNewBuffer(&dst->io->dump.pathPrefix, vStr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-layer")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        //ToDo: implement a centralized-list of supported layer's names.
                        if(
                           !NBString_strIsLike(vStr, "mask") &&
                           !NBString_strIsLike(vStr, "ssl") &&
                           !NBString_strIsLike(vStr, "base64") &&
                           !NBString_strIsLike(vStr, "dump")
                           )
                        {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, vStr); NBString_concat(&errsLst, "' value for '");
                            NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                        } else {
                            if(NBString_startsWith(&path, "/port/redir")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    STNBArray arr;
                                    NBArray_initWithSz(&arr, sizeof(char*), NULL, p->redir.layersSz + 1, 4, 0.1f);
                                    if(p->redir.layers != NULL){
                                        UI32 i; for(i = 0; i < p->redir.layersSz; i++){
                                            char* v = p->redir.layers[i];
                                            NBArray_addValue(&arr, v);
                                        }
                                        NBMemory_free(p->redir.layers);
                                    }
                                    //add value
                                    {
                                        char* v = NBString_strNewBuffer(vStr);
                                        NBArray_addValue(&arr, v);
                                        //resign to buffer
                                        p->redir.layers     = NBArray_dataPtr(&arr, char*);
                                        p->redir.layersSz   = (UI32)arr.use;
                                        NBArray_resignToBuffer(&arr);
                                    }
                                    NBArray_release(&arr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/port")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    STNBArray arr;
                                    NBArray_initWithSz(&arr, sizeof(char*), NULL, p->layersSz + 1, 4, 0.1f);
                                    if(p->layers != NULL){
                                        UI32 i; for(i = 0; i < p->layersSz; i++){
                                            char* v = p->layers[i];
                                            NBArray_addValue(&arr, v);
                                        }
                                        NBMemory_free(p->layers);
                                    }
                                    //add value
                                    {
                                        char* v = NBString_strNewBuffer(vStr);
                                        NBArray_addValue(&arr, v);
                                        //resign to buffer
                                        p->layers   = NBArray_dataPtr(&arr, char*);
                                        p->layersSz = (UI32)arr.use;
                                        NBArray_resignToBuffer(&arr);
                                    }
                                    NBArray_release(&arr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else if(NBString_startsWith(&path, "/io/redir")){
                                if(dst->io != NULL){
                                    STTNCoreCfgPort* p = dst->io;
                                    STNBArray arr;
                                    NBArray_initWithSz(&arr, sizeof(char*), NULL, p->redir.layersSz + 1, 4, 0.1f);
                                    if(p->redir.layers != NULL){
                                        UI32 i; for(i = 0; i < p->redir.layersSz; i++){
                                            char* v = p->redir.layers[i];
                                            NBArray_addValue(&arr, v);
                                        }
                                        NBMemory_free(p->redir.layers);
                                    }
                                    //add value
                                    {
                                        char* v = NBString_strNewBuffer(vStr);
                                        NBArray_addValue(&arr, v);
                                        //resign to buffer
                                        p->redir.layers     = NBArray_dataPtr(&arr, char*);
                                        p->redir.layersSz   = (UI32)arr.use;
                                        NBArray_resignToBuffer(&arr);
                                    }
                                    NBArray_release(&arr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else if(NBString_startsWith(&path, "/io")){
                                if(dst->io != NULL){
                                    STTNCoreCfgPort* p = dst->io;
                                    STNBArray arr;
                                    NBArray_initWithSz(&arr, sizeof(char*), NULL, p->layersSz + 1, 4, 0.1f);
                                    if(p->layers != NULL){
                                        UI32 i; for(i = 0; i < p->layersSz; i++){
                                            char* v = p->layers[i];
                                            NBArray_addValue(&arr, v);
                                        }
                                        NBMemory_free(p->layers);
                                    }
                                    //add value
                                    {
                                        char* v = NBString_strNewBuffer(vStr);
                                        NBArray_addValue(&arr, v);
                                        //resign to buffer
                                        p->layers   = NBArray_dataPtr(&arr, char*);
                                        p->layersSz = (UI32)arr.use;
                                        NBArray_resignToBuffer(&arr);
                                    }
                                    NBArray_release(&arr);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, io not set yet.");
                                }
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-ssl")){
                    if(NBString_startsWith(&path, "/port/redir")){
                        NBString_set(&path, "/port/redir/ssl");
                    } else if(NBString_startsWith(&path, "/port")){
                        NBString_set(&path, "/port/ssl");
                    } else if(NBString_startsWith(&path, "/io/redir")){
                        NBString_set(&path, "/io/redir/ssl");
                    } else if(NBString_startsWith(&path, "/io")){
                        NBString_set(&path, "/io/ssl");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-mask")){
                    if(NBString_startsWith(&path, "/port/redir")){
                        NBString_set(&path, "/port/redir/mask");
                    } else if(NBString_startsWith(&path, "/port")){
                        NBString_set(&path, "/port/mask");
                    } else if(NBString_startsWith(&path, "/io/redir")){
                        NBString_set(&path, "/io/redir/mask");
                    } else if(NBString_startsWith(&path, "/io")){
                        NBString_set(&path, "/io/mask");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-dump")){
                    if(NBString_startsWith(&path, "/port/redir")){
                        NBString_set(&path, "/port/redir/dump");
                    } else if(NBString_startsWith(&path, "/port")){
                        NBString_set(&path, "/port/dump");
                    } else if(NBString_startsWith(&path, "/io/redir")){
                        NBString_set(&path, "/io/redir/dump");
                    } else if(NBString_startsWith(&path, "/io")){
                        NBString_set(&path, "/io/dump");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-cert")){
                    if(NBString_startsWith(&path, "/port/redir/ssl")){
                        NBString_set(&path, "/port/redir/ssl/cert");
                    } else if(NBString_startsWith(&path, "/port/ssl")){
                        NBString_set(&path, "/port/ssl/cert");
                    } else if(NBString_startsWith(&path, "/io/redir/ssl")){
                        NBString_set(&path, "/io/redir/ssl/cert");
                    } else if(NBString_startsWith(&path, "/io/ssl")){
                        NBString_set(&path, "/io/ssl/cert");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-isRequested")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            if(NBString_startsWith(&path, "/port/ssl/cert")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    p->ssl.cert.isRequested = (v != 0 ? TRUE : FALSE);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-isRequired")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            if(NBString_startsWith(&path, "/port/ssl/cert")){
                                if(dst->portsSz > 0){
                                    STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                    p->ssl.cert.isRequired = (v != 0 ? TRUE : FALSE);
                                } else {
                                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                    NBString_concat(&errsLst, "Internal error, empty port-list.");
                                }
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                            }
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-source")){
                    if(NBString_startsWith(&path, "/port/redir/ssl/cert")){
                        NBString_set(&path, "/port/redir/ssl/cert/source");
                    } else if(NBString_startsWith(&path, "/port/ssl/cert")){
                        NBString_set(&path, "/port/ssl/cert/source");
                    } else if(NBString_startsWith(&path, "/io/redir/ssl/cert")){
                        NBString_set(&path, "/io/redir/ssl/cert/source");
                    } else if(NBString_startsWith(&path, "/io/ssl/cert")){
                        NBString_set(&path, "/io/ssl/cert/source");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-key")){
                    if(NBString_startsWith(&path, "/port/redir/ssl/cert/source")){
                        NBString_set(&path, "/port/redir/ssl/cert/source/key");
                    } else if(NBString_startsWith(&path, "/port/ssl/cert/source")){
                        NBString_set(&path, "/port/ssl/cert/source/key");
                    } else if(NBString_startsWith(&path, "/io/redir/ssl/cert/source")){
                        NBString_set(&path, "/io/redir/ssl/cert/source/key");
                    } else if(NBString_startsWith(&path, "/io/ssl/cert/source")){
                        NBString_set(&path, "/io/ssl/cert/source/key");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-pass")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        if(NBString_startsWith(&path, "/port/redir/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.key.pass, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.key.pass, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.key.pass, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/redir/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->redir.ssl.cert.source.key.pass, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                        }
                    }
                } else if(NBString_strIsLike(arg, "-name")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        if(NBString_startsWith(&path, "/port/redir/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.ssl.cert.source.key.name, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/port/ssl/cert/source/key")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->ssl.cert.source.key.name, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else if(NBString_startsWith(&path, "/io/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->ssl.cert.source.key.name, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else if(NBString_startsWith(&path, "/io/redir/ssl/cert/source/key")){
                            if(dst->io != NULL){
                                NBString_strFreeAndNewBuffer(&dst->io->redir.ssl.cert.source.key.name, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, io not set yet.");
                            }
                        } else {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                        }
                    }
                } else if(NBString_strIsLike(arg, "-redir")){
                    if(NBString_startsWith(&path, "/port")){
                        NBString_set(&path, "/port/redir");
                    } else if(NBString_startsWith(&path, "/io")){
                        NBString_set(&path, "/io/redir");
                    } else {
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                    }
                } else if(NBString_strIsLike(arg, "-server")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        if(NBString_startsWith(&path, "/port/redir")){
                            if(dst->portsSz > 0){
                                STTNCoreCfgPort* p = &dst->ports[dst->portsSz - 1];
                                NBString_strFreeAndNewBuffer(&p->redir.server, argv[i + 1]); i++;
                            } else {
                                if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                                NBString_concat(&errsLst, "Internal error, empty port-list.");
                            }
                        } else {
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Unexpected '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' param.");
                        }
                    }
                } else {
                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                    NBString_concat(&errsLst, "Unexpected param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "'.");
                }
            }
        }
        //result
        r = (errsLst.length == 0);
        //copy errors-string
        if(dstErrLst != NULL){
            NBString_concat(dstErrLst, errsLst.str);
        }
        //
        NBString_release(&path);
        NBString_release(&errsLst);
    }
    return r;
}
    
BOOL TNCore_parseArgs(const int argc, const char* argv[], STTNCoreArgs* dst, STNBString* dstErrLst){
    BOOL r = FALSE;
    if(dst != NULL && argv != NULL && argc > 0){
        STNBString errsLst;
        NBString_init(&errsLst);
        {
            int i; for(i = 1; i < argc && errsLst.length == 0; i++){ //ignore arg[0], its the current file-path
                const char* arg = argv[i];
                if(NBString_strIsLike(arg, "-cfgStart")){
                    //parse cfg-params
                    const int iStart = i + 1;
                    int iEnd = iStart;
                    while(iEnd < argc){
                        if(NBString_strIsLike(argv[iEnd], "-cfgEnd")){
                            break;
                        }
                        iEnd++;
                    }
                    //parse cfg params
                    if(iStart < iEnd){
                        if(!TNCore_parseArgsCfg_((iEnd - iStart), &argv[iStart], &dst->cfg, &errsLst)){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Cfg-params failed.");
                            PRINTF_ERROR("TNCore, TNCore_parseArgsCfg_, failed.\n");
                        }
                    }
                    i = iEnd;
                } else if(NBString_strIsLike(arg, "-secsRunAndExit")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            dst->secsRunAndExit = v;
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-maxSecsWithoutConn")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            dst->maxSecsWithoutConn = v;
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-maxConnsAndExit")){
                    if((i + 1) >= argc){
                        if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                        NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' without value.");
                    } else {
                        const char* vStr = argv[i + 1];
                        BOOL parseSuccess = FALSE;
                        const UI32 v = NBNumParser_toUI32(vStr, &parseSuccess);
                        if(!parseSuccess){
                            if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                            NBString_concat(&errsLst, "Param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "' value is not numeric.");
                        } else {
                            dst->maxConnsAndExit = v;
                        }
                        i++;
                    }
                } else if(NBString_strIsLike(arg, "-printArgs")){
                    dst->printArgs = TRUE;
                } else if(NBString_strIsLike(arg, "-printCfg")){
                    dst->printCfg = TRUE;
                } else if(NBString_strIsLike(arg, "-help") || NBString_strIsLike(arg, "--help")){
                    dst->printHelp = TRUE;
                } else {
                    if(errsLst.length != 0) NBString_concatByte(&errsLst, '\n');
                    NBString_concat(&errsLst, "Unexpected param '"); NBString_concat(&errsLst, arg); NBString_concat(&errsLst, "'.");
                }
            }
        }
        //result
        r = (errsLst.length == 0);
        //copy errors-string
        if(dstErrLst != NULL){
            NBString_concat(dstErrLst, errsLst.str);
        }
        //
        NBString_release(&errsLst);
    }
    return r;
}

BOOL TNCore_concatArsgHelp(const char* exeFilename, STNBString* dst){
    BOOL r = FALSE;
    if(dst != NULL){
        NBString_concat(dst, "\n");
        NBString_concat(dst, "-cfgStart ... -cfgEnd, config-params (see below).\n");
        NBString_concat(dst, "-secsRunAndExit [secs], stop execution after [secs].\n");
        NBString_concat(dst, "-maxSecsWithoutConn [secs], stop execution after [secs] with no conns.\n");
        NBString_concat(dst, "-maxConnsAndExit [amm], stop receiving new conns after [amm] and exits after closed.\n");
        NBString_concat(dst, "-printArgs, prints parsed/loaded args.\n");
        NBString_concat(dst, "-printCfg, prints parsed/loaded cfg-args.\n");
        NBString_concat(dst, "-help | --help, prints help.\n");
        NBString_concat(dst, "\n");
        NBString_concat(dst, "Config params:\n");
        NBString_concat(dst, "-cfgStart, following params are parsed as config untill -cfgEnd appears.\n");
        NBString_concat(dst, "-CAs, following params are applied to the default certificate-authorities-list.\n");
        NBString_concat(dst, " | -path [path], adds one CA certificate to the default list.\n");
        NBString_concat(dst, " | -pay64 [base64], adds one CA certificate to the default list.\n");
        NBString_concat(dst, "-port [number], adds an in-port to the list, following params are applied to this port.\n");
        NBString_concat(dst, " | -layer [mask|ssl|base64], adds a layer to the inStack.\n");
        NBString_concat(dst, " | -ssl, following params are applied to inSsl context.\n");
        NBString_concat(dst, " |  | -cert, following params are applied to in-ssl-certificate.\n");
        NBString_concat(dst, " |  |  | -isRequested [0|1], port will request clients to provide an optional certificate.\n");
        NBString_concat(dst, " |  |  | -isRequired [0|1], port will drop connection if certificate was not provided.\n");
        NBString_concat(dst, " |  |  | -source, following params are applied to inSsl-certificate-source.\n");
        NBString_concat(dst, " |  |  |  | -path [path], defines the path for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  | -pay64 [base64], defines the payload for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  | -key, following params are applied to inSsl-key.\n");
        NBString_concat(dst, " |  |  |  |  | -path [path], defines the path of the inSsl-key-file.\n");
        NBString_concat(dst, " |  |  |  |  | -pay64 [base64], defines the payload for the inSsl-key.\n");
        NBString_concat(dst, " |  |  |  |  | -pass [pass], defines the password of the inSsl-key-file.\n");
        NBString_concat(dst, " |  |  |  |  | -name [name], defines an internal friendly name for the inSsl-key-file.\n");
        NBString_concat(dst, " |  | -CAs, following params are applied to the inSsl certificate-authorities-list.\n");
        NBString_concat(dst, " |  |  | -path [path], adds one CA certificate to the inSsl list.\n");
        NBString_concat(dst, " |  |  | -pay64 [base64], adds one CA certificate to the inSsl list.\n");
        NBString_concat(dst, " | -mask, following params are applied to inMasking context.\n");
        NBString_concat(dst, " |  | -seed [0-255], inMask seed value.\n");
        NBString_concat(dst, " | -dump, following params are applied to inDumping context.\n");
        NBString_concat(dst, " |  | -pathprefix [path], inDumping path value.\n");
        NBString_concat(dst, " | -redir, following params are applied to redirection of port's conns.\n");
        NBString_concat(dst, " |  | -server [server], defines the destination server.\n");
        NBString_concat(dst, " |  | -port [number], defines the destination port.\n");
        NBString_concat(dst, " |  | -layer [mask|ssl|base64], adds a layer to the outStack.\n");
        NBString_concat(dst, " |  | -ssl, following params are applied to outSsl context.\n");
        NBString_concat(dst, " |  |  | -cert, following params are applied to outSsl-certificate.\n");
        NBString_concat(dst, " |  |  |  | -source, following params are applied to outSsl-certificate-source.\n");
        NBString_concat(dst, " |  |  |  |  | -path [path], defines the path for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  |  | -key, following params are applied to outSsl-certificate-key.\n");
        NBString_concat(dst, " |  |  |  |  |  | -path [path], defines the path of the outSsl-key-file.\n");
        NBString_concat(dst, " |  |  |  |  |  | -pass [pass], defines the password of the outSsl-key-file.\n");
        NBString_concat(dst, " |  |  |  |  |  | -name [name], defines an internal friendly name for the outSsl-key-file.\n");
        NBString_concat(dst, " |  |  | -CAs, following params are applied to the outSsl certificate-authorities-list.\n");
        NBString_concat(dst, " |  |  |  | -path [path], adds one CA certificate to the outSsl list.\n");
        NBString_concat(dst, " |  |  |  | -pay64 [base64], adds one CA certificate to the outSsl list.\n");
        NBString_concat(dst, " |  | -mask, following params are applied to outMasking context.\n");
        NBString_concat(dst, " |  |  | -seed, outMask seed value.\n");
        NBString_concat(dst, " |  | -dump, following params are applied to outDumping context.\n");
        NBString_concat(dst, " |  |  | -pathPrefix [path], outDumping path value.\n");
        NBString_concat(dst, "-io, enables stdin/stdout processing, following params are applied to this io.\n");
        NBString_concat(dst, " | -layer [mask|ssl|base64], adds a layer to the stdin.\n");
        NBString_concat(dst, " | -ssl, following params are applied to stdin context.\n");
        NBString_concat(dst, " |  | -cert, following params are applied to stdin-certificate.\n");
        NBString_concat(dst, " |  |  | -source, following params are applied to stdin-certificate-source.\n");
        NBString_concat(dst, " |  |  |  | -path [path], defines the path for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  | -pay64 [base64], defines the payload for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  | -key, following params are applied to stdin-key.\n");
        NBString_concat(dst, " |  |  |  |  | -path [path], defines the path of the stdin-key-file.\n");
        NBString_concat(dst, " |  |  |  |  | -pay64 [base64], defines the payload for the stdin-key.\n");
        NBString_concat(dst, " |  |  |  |  | -pass [pass], defines the password of the stdin-key-file.\n");
        NBString_concat(dst, " |  |  |  |  | -name [name], defines an internal friendly name for the stdin-key-file.\n");
        NBString_concat(dst, " | -mask, following params are applied to stdinMasking context.\n");
        NBString_concat(dst, " |  | -seed [0-255], stdinMask seed value.\n");
        NBString_concat(dst, " | -dump, following params are applied to stdinDumping context.\n");
        NBString_concat(dst, " |  | -pathPrefix [path], stdinDumping path value.\n");
        NBString_concat(dst, " | -redir, following params are applied to redirection to stdout.\n");
        NBString_concat(dst, " |  | -layer [mask|ssl|base64], adds a layer to the stdout.\n");
        NBString_concat(dst, " |  | -ssl, following params are applied to stdout context.\n");
        NBString_concat(dst, " |  |  | -cert, following params are applied to stdout-certificate.\n");
        NBString_concat(dst, " |  |  |  | -source, following params are applied to stdout-certificate-source.\n");
        NBString_concat(dst, " |  |  |  |  | -path [path], defines the path for the certificate to use.\n");
        NBString_concat(dst, " |  |  |  |  | -key, following params are applied to stdout-certificate-key.\n");
        NBString_concat(dst, " |  |  |  |  |  | -path [path], defines the path of the stdout-key-file.\n");
        NBString_concat(dst, " |  |  |  |  |  | -pass [pass], defines the password of the stdout-key-file.\n");
        NBString_concat(dst, " |  |  |  |  |  | -name [name], defines an internal friendly name for the stdout-key-file.\n");
        NBString_concat(dst, " |  | -mask, following params are applied to stdoutMasking context.\n");
        NBString_concat(dst, " |  |  | -seed, stdoutMask seed value.\n");
        NBString_concat(dst, " |  | -dump, following params are applied to stdoutDumping context.\n");
        NBString_concat(dst, " |  |  | -pathPrefix [path], stdoutDumping path value.\n");
        NBString_concat(dst, "-cfgEnd, following params are parsed as non-cfg params.\n");
        NBString_concat(dst, "\n");
        //example
        {
            STNBString filename;
            NBString_init(&filename);
            //filename
            {
                const SI32 laskSlash0 = NBString_strLastIndexOf(exeFilename, "/", NBString_strLenBytes(exeFilename));
                const SI32 laskSlash1 = NBString_strLastIndexOf(exeFilename, "\\", NBString_strLenBytes(exeFilename));
                const SI32 lastSlashPos = (laskSlash0 >= 0 && laskSlash1 >= 0 ? (laskSlash0 < laskSlash1 ? laskSlash0 : laskSlash1) : laskSlash0 >= 0 ? laskSlash0 : laskSlash1);
                if(lastSlashPos < 0){
                    NBString_set(&filename, exeFilename);
                } else {
                    NBString_set(&filename, &exeFilename[lastSlashPos + 1]);
                }
            }
            //
            NBString_concat(dst, "Examples:\n");
            //
            NBString_concat(dst, "\n");
            NBString_concat(dst, "This command opens a port 8089 for masking and redirecting to port 8090:\n");
            NBString_concat(dst, filename.str); NBString_concat(dst, " -cfgStart -port 8089 -redir -server localhost -port 8090 -layer mask -mask -seed 99 -cfgEnd\n");
            //
            NBString_concat(dst, "\n");
            NBString_concat(dst, "This command opens a port 8090 for unmasking and redirecting to port google:443:\n");
            NBString_concat(dst, filename.str); NBString_concat(dst, " -maxConnsAndExit 1 -cfgStart -port 8090 -layer mask -mask -seed 99 -redir -server www.google.com.ni -port 443\n");
            //cfg-params
            NBString_concat(dst, "\n");
            NBString_concat(dst, "This command opens a port 8089 for ssl-encryption, masking, and redirecting to port 8090:\n");
            NBString_concat(dst, filename.str); NBString_concat(dst, " -cfgStart -CAs -path file.cert -path \"file 2.cert\" -port 8089 -redir -server \"remote.com\" -port 8090 -layer ssl -layer mask -ssl -cert -source -path \"file 3.cert\" -key -path \"file 4.key\" -CAs -path \"file 5.cert\" -cfgEnd\n");
            //
            NBString_concat(dst, "\n");
            NBString_concat(dst, "This command reads stdin and sends the masked data to stdout in base64:\n");
            NBString_concat(dst, filename.str); NBString_concat(dst, " -cfgStart -io -redir -layer mask -layer base64 -mask -seed 199 -cfgEnd\n");
            //
            NBString_concat(dst, "\n");
            NBString_concat(dst, "This command reads stdin in base64 and sends the unmasked data to stdout:\n");
            NBString_concat(dst, filename.str); NBString_concat(dst, " -cfgStart -io -layer mask -layer base64 -mask -seed 199 -redir -cfgEnd\n");
            //
            NBString_release(&filename);
        }
        r = TRUE;
    }
    return r;
}

BOOL TNCore_setPollster(STTNCoreRef ref, STNBIOPollsterSyncRef pollSync){    //when one pollster only
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBASSERT(opq != NULL)
    NBObject_lock(opq);
    if(!opq->isPrepared && !opq->isListening){
        //set
        NBIOPollsterSync_set(&opq->pollster.sync, &pollSync);
        //
        r = TRUE;
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCore_setPollstersProvider(STTNCoreRef ref, STNBIOPollstersProviderRef provider){ //when multiple pollsters
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBASSERT(opq != NULL)
    NBObject_lock(opq);
    if(!opq->isPrepared && !opq->isListening){
        //set
        NBIOPollstersProvider_set(&opq->pollster.provider, &provider);
        //
        r = TRUE;
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCore_setParentStopFlag(STTNCoreRef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
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

BOOL TNCore_prepare(STTNCoreRef ref, const STTNCoreArgs* args){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    const STTNCoreCfg* cfg = (args == NULL ? NULL : &args->cfg);
    NBObject_lock(opq);
    if(!opq->isPrepared && !opq->isListening && !opq->isRunning){
        STNBIOPollsterSyncRef pollSync = opq->pollster.sync;
        //
        STTNCorePortLstnrItf itf;
        NBMemory_setZeroSt(itf, STTNCorePortLstnrItf);
        itf.portConnArrived = TNCore_portConnArrivedOpq_;
        //
        r = TRUE;
        //load CAs
        if(r && cfg != NULL){
            STNBArray arr;
            NBArray_init(&arr, sizeof(STNBX509), NULL);
            {
                UI32 i; for(i = 0; i < cfg->casSz && r; i++){
                    const STTNCoreCfgSslCertSrc* src = &cfg->cas[i];
                    if(!NBString_strIsEmpty(src->pay64)){
                        STNBString pay;
                        NBString_init(&pay);
                        if(!NBBase64_decode(&pay, src->pay64)){
                            PRINTF_ERROR("TNCore, NBBase64_decode failed for root CA-cert.\n");
                            r = FALSE;
                        } else {
                            STNBX509 c;
                            NBX509_init(&c);
                            if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                PRINTF_ERROR("TNCore, NBX509_createFromDERBytes failed for root CA-cert.\n");
                                r = FALSE;
                                NBX509_release(&c);
                            } else {
                                //PRINTF_INFO("TNCore, NBX509_createFromDERBytes success (base64).\n");
                                NBArray_addValue(&arr, c);
                            }
                        }
                        NBString_release(&pay);
                    } else if(!NBString_strIsEmpty(src->path)){
                        STNBFileRef f = NBFile_alloc(NULL);
                        if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                            PRINTF_ERROR("TNCore, NBFile_open failed for root CA-cert: '%s'.\n", src->path);
                            r = FALSE;
                        } else {
                            NBFile_lock(f);
                            {
                                STNBX509 c;
                                NBX509_init(&c);
                                if(!NBX509_createFromDERFile(&c, f)){
                                    PRINTF_ERROR("TNCore, NBX509_createFromDERFile failed for root CA-cert: '%s'.\n", src->path);
                                    r = FALSE;
                                    NBX509_release(&c);
                                } else {
                                    //PRINTF_INFO("TNCore, NBX509_createFromDERFile success (file-path).\n");
                                    NBArray_addValue(&arr, c);
                                }
                            }
                            NBFile_unlock(f);
                        }
                        NBFile_release(&f);
                    }
                }
            }
            //consume
            if(r){
                //remove current
                {
                    SI32 i; for(i = 0; i < opq->CAs.arr.use; i++){
                        STNBX509* c = NBArray_itmPtrAtIndex(&opq->CAs.arr, STNBX509, i);
                        NBX509_release(c);
                    }
                    NBArray_empty(&opq->CAs.arr);
                }
                //move from newArr to current
                {
                    SI32 i; for(i = 0; i < arr.use; i++){
                        STNBX509* c = NBArray_itmPtrAtIndex(&arr, STNBX509, i);
                        NBArray_addValue(&opq->CAs.arr, *c);
                    }
                    NBArray_empty(&arr);
                }
            }
            //release (not consumed ones)
            {
                SI32 i; for(i = 0; i < arr.use; i++){
                    STNBX509* c = NBArray_itmPtrAtIndex(&arr, STNBX509, i);
                    NBX509_release(c);
                }
                NBArray_empty(&arr);
            }
            NBArray_release(&arr);
        }
        //define pollster
        if(r){
            //get a pollster from provider
            if(!NBIOPollsterSync_isSet(pollSync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                pollSync = opq->pollster.sync = NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
            }
            //create pollster
            if(!NBIOPollsterSync_isSet(pollSync)){
                pollSync = opq->pollster.sync = NBIOPollsterSync_alloc(NULL);
                opq->pollster.def = NBIOPollster_alloc(NULL);
            }
        }
        //Add ports to array
        if(r && cfg != NULL){
            UI32 i; for(i = 0; i < cfg->portsSz && r; i++){
                const STTNCoreCfgPort* cfgPort = &cfg->ports[i];
                STTNCorePortRef p = TNCorePort_alloc(NULL);
                if (!TNCorePort_setParentStopFlag(p, &opq->stopFlag)) {
                    PRINTF_ERROR("TNCore, TNCorePort_setParentStopFlag failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_setPollster(p, pollSync)){
                    PRINTF_ERROR("TNCore, TNCorePort_setPollster failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_setListener(p, &itf, opq)){
                    PRINTF_ERROR("TNCore, TNCorePort_setListener failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_prepare(p, cfgPort, NBArray_dataPtr(&opq->CAs.arr, STNBX509), opq->CAs.arr.use)){
                    PRINTF_ERROR("TNCore, TNCorePort_prepare failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else {
                    //add
                    TNCorePort_retain(p);
                    NBArray_addValue(&opq->ports.arr, p);
                }
                TNCorePort_release(&p);
                TNCorePort_null(&p);
            }
        }
        //clone args
        if(r && args != NULL){
            NBStruct_stRelease(TNCoreArgs_getSharedStructMap(), &opq->args, sizeof(opq->args));
            NBStruct_stClone(TNCoreArgs_getSharedStructMap(), args, sizeof(*args), &opq->args, sizeof(opq->args));
        }
        //flags as prepared
        if(r){
            opq->isPrepared = TRUE;
        }
        //revert if failed
        if(!r){
            SI32 i; for(i = 0; i < opq->ports.arr.use; i++){
                STTNCorePortRef p = NBArray_itmValueAtIndex(&opq->ports.arr, STTNCorePortRef, i);
                if(TNCorePort_isSet(p)){
                    TNCorePort_release(&p); //internally will be stopped
                    TNCorePort_null(&p);
                }
            }
            NBArray_empty(&opq->ports.arr);
        }
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCore_addCA(STTNCoreRef ref, const STTNCoreCfgSslCertSrc* cfg){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    if(cfg != NULL){
        NBObject_lock(opq);
        if(!opq->isPrepared && !opq->isListening && !opq->isRunning){
            r = TRUE;
            //load CAs
            if(r){
                const STTNCoreCfgSslCertSrc* src = cfg;
                if(!NBString_strIsEmpty(src->pay64)){
                    STNBString pay;
                    NBString_init(&pay);
                    if(!NBBase64_decode(&pay, src->pay64)){
                        PRINTF_ERROR("TNCore, NBBase64_decode failed for root CA-cert.\n");
                        r = FALSE;
                    } else {
                        STNBX509 c;
                        NBX509_init(&c);
                        if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                            PRINTF_ERROR("TNCore, NBX509_createFromDERBytes failed for root CA-cert.\n");
                            r = FALSE;
                            NBX509_release(&c);
                        } else {
                            //PRINTF_INFO("TNCore, NBX509_createFromDERBytes success (base64).\n");
                            NBArray_addValue(&opq->CAs.arr, c);
                        }
                    }
                    NBString_release(&pay);
                } else if(!NBString_strIsEmpty(src->path)){
                    STNBFileRef f = NBFile_alloc(NULL);
                    if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                        PRINTF_ERROR("TNCore, NBFile_open failed for root CA-cert: '%s'.\n", src->path);
                        r = FALSE;
                    } else {
                        NBFile_lock(f);
                        {
                            STNBX509 c;
                            NBX509_init(&c);
                            if(!NBX509_createFromDERFile(&c, f)){
                                PRINTF_ERROR("TNCore, NBX509_createFromDERFile failed for root CA-cert: '%s'.\n", src->path);
                                r = FALSE;
                                NBX509_release(&c);
                            } else {
                                //PRINTF_INFO("TNCore, NBX509_createFromDERFile success (file-path).\n");
                                NBArray_addValue(&opq->CAs.arr, c);
                            }
                        }
                        NBFile_unlock(f);
                    }
                    NBFile_release(&f);
                }
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNCore_addCAs(STTNCoreRef ref, const STTNCoreCfgCAs* cfg){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    if(cfg != NULL){
        NBObject_lock(opq);
        if(!opq->isPrepared && !opq->isListening && !opq->isRunning){
            r = TRUE;
            //load CAs
            if(r){
                UI32 i; for(i = 0 ; i < cfg->casSz && r; i++){
                    const STTNCoreCfgSslCertSrc* src = &cfg->cas[i];
                    if(!NBString_strIsEmpty(src->pay64)){
                        STNBString pay;
                        NBString_init(&pay);
                        if(!NBBase64_decode(&pay, src->pay64)){
                            PRINTF_ERROR("TNCore, NBBase64_decode failed for root CA-cert.\n");
                            r = FALSE;
                        } else {
                            STNBX509 c;
                            NBX509_init(&c);
                            if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                PRINTF_ERROR("TNCore, NBX509_createFromDERBytes failed for root CA-cert.\n");
                                r = FALSE;
                                NBX509_release(&c);
                            } else {
                                //PRINTF_INFO("TNCore, NBX509_createFromDERBytes success (base64).\n");
                                NBArray_addValue(&opq->CAs.arr, c);
                            }
                        }
                        NBString_release(&pay);
                    } else if(!NBString_strIsEmpty(src->path)){
                        STNBFileRef f = NBFile_alloc(NULL);
                        if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                            PRINTF_ERROR("TNCore, NBFile_open failed for root CA-cert: '%s'.\n", src->path);
                            r = FALSE;
                        } else {
                            NBFile_lock(f);
                            {
                                STNBX509 c;
                                NBX509_init(&c);
                                if(!NBX509_createFromDERFile(&c, f)){
                                    PRINTF_ERROR("TNCore, NBX509_createFromDERFile failed for root CA-cert: '%s'.\n", src->path);
                                    r = FALSE;
                                    NBX509_release(&c);
                                } else {
                                    //PRINTF_INFO("TNCore, NBX509_createFromDERFile success (file-path).\n");
                                    NBArray_addValue(&opq->CAs.arr, c);
                                }
                            }
                            NBFile_unlock(f);
                        }
                        NBFile_release(&f);
                    }
                }
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNCore_addPort(STTNCoreRef ref, const STTNCoreCfgPort* cfg){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    if(cfg != NULL){
        NBObject_lock(opq);
        if(!opq->isPrepared && !opq->isListening && !opq->isRunning){
            STNBIOPollsterSyncRef pollSync = opq->pollster.sync;
            //
            STTNCorePortLstnrItf itf;
            NBMemory_setZeroSt(itf, STTNCorePortLstnrItf);
            itf.portConnArrived = TNCore_portConnArrivedOpq_;
            //
            r = TRUE;
            //define pollster
            if(r){
                //get a pollster from provider
                if(!NBIOPollsterSync_isSet(pollSync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                    pollSync = opq->pollster.sync = NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                }
                //create pollsync
                if(!NBIOPollsterSync_isSet(pollSync)){
                    pollSync = opq->pollster.sync = NBIOPollsterSync_alloc(NULL);
                    opq->pollster.def = NBIOPollster_alloc(NULL);
                }
            }
            //Add ports to array
            if(r){
                const STTNCoreCfgPort* cfgPort = cfg;
                STTNCorePortRef p = TNCorePort_alloc(NULL);
                if (!TNCorePort_setParentStopFlag(p, &opq->stopFlag)) {
                    PRINTF_ERROR("TNCore, TNCorePort_setParentStopFlag failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_setPollster(p, pollSync)){
                    PRINTF_ERROR("TNCore, TNCorePort_setPollster failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_setListener(p, &itf, opq)){
                    PRINTF_ERROR("TNCore, TNCorePort_setListener failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else if(!TNCorePort_prepare(p, cfgPort, NBArray_dataPtr(&opq->CAs.arr, STNBX509), opq->CAs.arr.use)){
                    PRINTF_ERROR("TNCore, TNCorePort_prepare failed for port(%u).\n", cfgPort->port);
                    r = FALSE;
                } else {
                    //add
                    TNCorePort_retain(p);
                    NBArray_addValue(&opq->ports.arr, p);
                }
                TNCorePort_release(&p);
                TNCorePort_null(&p);
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNCore_addPorts(STTNCoreRef ref, const STTNCoreCfgPorts* cfg){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    if(cfg != NULL){
        NBObject_lock(opq);
        if(!opq->isPrepared && !opq->isListening && !opq->isRunning){
            STNBIOPollsterSyncRef pollSync = opq->pollster.sync;
            //
            STTNCorePortLstnrItf itf;
            NBMemory_setZeroSt(itf, STTNCorePortLstnrItf);
            itf.portConnArrived = TNCore_portConnArrivedOpq_;
            //
            r = TRUE;
            //define pollster
            if(r){
                //get a pollster from provider
                if(!NBIOPollsterSync_isSet(pollSync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                    pollSync = opq->pollster.sync = NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                }
                //create pollsync
                if(!NBIOPollsterSync_isSet(pollSync)){
                    pollSync = opq->pollster.sync = NBIOPollsterSync_alloc(NULL);
                    opq->pollster.def = NBIOPollster_alloc(NULL);
                }
            }
            //Add ports to array
            if(r){
                UI32 i; for(i = 0; i < cfg->portsSz && r; i++){
                    const STTNCoreCfgPort* cfgPort = &cfg->ports[i];
                    STTNCorePortRef p = TNCorePort_alloc(NULL);
                    if (!TNCorePort_setParentStopFlag(p, &opq->stopFlag)) {
                        PRINTF_ERROR("TNCore, TNCorePort_setParentStopFlag failed for port(%u).\n", cfgPort->port);
                        r = FALSE;
                    } else if(!TNCorePort_setPollster(p, pollSync)){
                        PRINTF_ERROR("TNCore, TNCorePort_setPollster failed for port(%u).\n", cfgPort->port);
                        r = FALSE;
                    } else if(!TNCorePort_setListener(p, &itf, opq)){
                        PRINTF_ERROR("TNCore, TNCorePort_setListener failed for port(%u).\n", cfgPort->port);
                        r = FALSE;
                    } else if(!TNCorePort_prepare(p, cfgPort, NBArray_dataPtr(&opq->CAs.arr, STNBX509), opq->CAs.arr.use)){
                        PRINTF_ERROR("TNCore, TNCorePort_prepare failed for port(%u).\n", cfgPort->port);
                        r = FALSE;
                    } else {
                        //add
                        TNCorePort_retain(p);
                        NBArray_addValue(&opq->ports.arr, p);
                    }
                    TNCorePort_release(&p);
                    TNCorePort_null(&p);
                }
            }
        }
        NBObject_unlock(opq);
    }
    return r;
}

BOOL TNCore_prepareLyrsInvOrderOpq_(STTNCoreOpq* opq, const STTNCoreCfgPort* cfg, STNBArray* dst /*STTNLyrLstnr*/){
    BOOL r = TRUE;
    const SI32 szbefore = dst->use;
    //add redir layers (in inverse order)
    if(r){
        SI32 i; for(i = (SI32)cfg->redir.layersSz - 1; i >= 0 && r; i--){
            const char* lyrType = cfg->redir.layers[i];
            if(NBString_strIsLike(lyrType, "mask")){
                STTNLyrLstnr lstnr;
                STTNLyrMaskRef s = TNLyrMask_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrMask_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrMask_setParentStopFlag failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrMask_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrMask_getLyrItf failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrMask_prepare(s, ENTNLyrFlow_FromLwr, cfg->redir.mask.seed)){
                    PRINTF_ERROR("TNCore, TNLyrMask_start failed for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for unmask layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrMask_release(&s);
                TNLyrMask_null(&s);
            } else if(NBString_strIsLike(lyrType, "base64")){
                STTNLyrLstnr lstnr;
                STTNLyrBase64Ref s = TNLyrBase64_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrBase64_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_setParentStopFlag failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrBase64_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_getLyrItf failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrBase64_prepare(s, ENTNLyrFlow_FromLwr)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_start failed for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for unmask layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrBase64_release(&s);
                TNLyrBase64_null(&s);
            } else if(NBString_strIsLike(lyrType, "dump")){
                STTNLyrLstnr lstnr;
                STTNLyrDumpRef s = TNLyrDump_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrDump_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrDump_setParentStopFlag failed for dump layer.\n");
                    r = FALSE;
                } else if(!TNLyrDump_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrDump_getLyrItf failed for dump layer.\n");
                    r = FALSE;
                } else if(!TNLyrDump_prepare(s, ENTNLyrFlow_FromLwr, cfg->dump.pathPrefix)){
                    PRINTF_ERROR("TNCore, TNLyrDump_start failed for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for dump layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrDump_release(&s);
                TNLyrDump_null(&s);
            } else {
                //ToDo: implement "ssl"
                PRINTF_ERROR("TNCore, unsuported layer '%s'.\n", lyrType);
                r = FALSE;
            }
        }
    }
    //add layers (in inverse order)
    if(r){
        SI32 i; for(i = (SI32)cfg->layersSz - 1; i >= 0 && r; i--){
            const char* lyrType = cfg->layers[i];
            if(NBString_strIsLike(lyrType, "mask")){
                STTNLyrLstnr lstnr;
                STTNLyrMaskRef s = TNLyrMask_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrMask_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrMask_setParentStopFlag failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrMask_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrMask_getLyrItf failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrMask_prepare(s, ENTNLyrFlow_FromUp, cfg->mask.seed)){
                    PRINTF_ERROR("TNCore, TNLyrMask_start failed for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for unmask layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrMask_release(&s);
                TNLyrMask_null(&s);
            } else if(NBString_strIsLike(lyrType, "base64")){
                STTNLyrLstnr lstnr;
                STTNLyrBase64Ref s = TNLyrBase64_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrBase64_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_setParentStopFlag failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrBase64_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_getLyrItf failed for unmask layer.\n");
                    r = FALSE;
                } else if(!TNLyrBase64_prepare(s, ENTNLyrFlow_FromUp)){
                    PRINTF_ERROR("TNCore, TNLyrBase64_start failed for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for unmask layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for unmask layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrBase64_release(&s);
                TNLyrBase64_null(&s);
            } else if(NBString_strIsLike(lyrType, "dump")){
                STTNLyrLstnr lstnr;
                STTNLyrDumpRef s = TNLyrDump_alloc(NULL);
                NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                if(!TNLyrDump_setParentStopFlag(s, &opq->stopFlag)){
                    PRINTF_ERROR("TNCore, TNLyrDump_setParentStopFlag failed for dump layer.\n");
                    r = FALSE;
                } else if(!TNLyrDump_getLyrItf(s, &lstnr)){
                    PRINTF_ERROR("TNCore, TNLyrDump_getLyrItf failed for dump layer.\n");
                    r = FALSE;
                } else if(!TNLyrDump_prepare(s, ENTNLyrFlow_FromUp, cfg->dump.pathPrefix)){
                    PRINTF_ERROR("TNCore, TNLyrDump_start failed for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRetain == NULL){
                    PRINTF_ERROR("TNCore, lyrRetain method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrRelease == NULL){
                    PRINTF_ERROR("TNCore, lyrRelease method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrSetNext == NULL){
                    PRINTF_ERROR("TNCore, lyrSetNext method is NULL for dump layer.\n");
                    r = FALSE;
                } else if(lstnr.itf.lyrStart == NULL){
                    PRINTF_ERROR("TNCore, lyrStart method is NULL for dump layer.\n");
                    r = FALSE;
                } else {
                    (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                    NBArray_addValue(dst, lstnr);
                }
                TNLyrDump_release(&s);
                TNLyrDump_null(&s);
            } else {
                //ToDo: implement "ssl"
                PRINTF_ERROR("TNCore, unsuported layer '%s'.\n", lyrType);
                r = FALSE;
            }
        }
    }
    //revert if failed
    if(!r){
        SI32 i; for(i = szbefore; i < dst->use; i++){
            STTNLyrLstnr* s = NBArray_itmPtrAtIndex(dst, STTNLyrLstnr, i);
            if(s->itf.lyrRelease != NULL){
                (*s->itf.lyrRelease)(s->usrParam);
            }
        }
        NBArray_removeItemsAtIndex(dst, szbefore, dst->use - szbefore);
    }
    return r;
}

BOOL TNCore_startListening(STTNCoreRef ref){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBObject_lock(opq);
    if(opq->isPrepared && !opq->isListening && !opq->isRunning){
        r = TRUE;
        //ports (start listening)
        if(r){
            SI32 i; for(i = 0; i < opq->ports.arr.use && r; i++){
                STTNCorePortRef p = NBArray_itmValueAtIndex(&opq->ports.arr, STTNCorePortRef, i);
                if(!TNCorePort_startListening(p)){
                    PRINTF_ERROR("TNCore, TNCorePort_startListening failed for port(%u).\n", TNCorePort_getPort(p));
                    r = FALSE;
                } else {
                    PRINTF_INFO("TNCore, listening port(%u).\n", TNCorePort_getPort(p));
                }
            }
        }
        //stdin/out (start connection)
        if(r && opq->args.cfg.io != NULL){
            STNBArray lstnrs; //STTNLyrLstnr
            NBArray_init(&lstnrs, sizeof(STTNLyrLstnr), NULL);
            r = TRUE;
            //Build layers-stack (in inverse order)
            {
                const STTNCoreCfgPort* cfg = opq->args.cfg.io;
                if(!TNCore_prepareLyrsInvOrderOpq_(opq, cfg, &lstnrs)){
                    r = FALSE;
                } else {
                    //stdout
                    if(r){
                        STTNLyrLstnr lstnr;
                        STTNLyrIORef s = TNLyrIO_alloc(NULL);
                        NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                        if(!TNLyrIO_setPollsterSync(s, opq->pollster.sync)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setPollsterSync failed for stdout layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_setParentStopFlag(s, &opq->stopFlag)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setParentStopFlag failed for stdout layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_getLyrItf(s, &lstnr)){
                            PRINTF_ERROR("TNCore, TNLyrIO_getLyrItf failed for stdout layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_prepareAsStdOut(s)){
                            PRINTF_ERROR("TNCore, TNLyrIO_startAsStdOut failed for stdout layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRetain == NULL){
                            PRINTF_ERROR("TNCore, lyrRetain method is NULL for stdout layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRelease == NULL){
                            PRINTF_ERROR("TNCore, lyrRelease method is NULL for stdout layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrSetNext == NULL){
                            PRINTF_ERROR("TNCore, lyrSetNext method is NULL for stdout layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrStart == NULL){
                            PRINTF_ERROR("TNCore, lyrStart method is NULL for stdout layer.\n");
                            r = FALSE;
                        } else {
                            (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                            //add as first lyr
                            NBArray_addItemsAtIndex(&lstnrs, 0, &lstnr, sizeof(lstnr), 1);
                        }
                        TNLyrIO_release(&s);
                        TNLyrIO_null(&s);
                    }
                    //stdin
                    if(r){
                        STTNLyrLstnr lstnr;
                        STTNLyrIORef s = TNLyrIO_alloc(NULL);
                        NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                        if(!TNLyrIO_setPollsterSync(s, opq->pollster.sync)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setPollsterSync failed for stdin layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_setParentStopFlag(s, &opq->stopFlag)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setParentStopFlag failed for stdin layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_getLyrItf(s, &lstnr)){
                            PRINTF_ERROR("TNCore, TNLyrIO_getLyrItf failed for stdin layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_prepareAsStdIn(s)){
                            PRINTF_ERROR("TNCore, TNLyrIO_startConnecting failed for stdin layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRetain == NULL){
                            PRINTF_ERROR("TNCore, lyrRetain method is NULL for stdin layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRelease == NULL){
                            PRINTF_ERROR("TNCore, lyrRelease method is NULL for stdin layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrSetNext == NULL){
                            PRINTF_ERROR("TNCore, lyrSetNext method is NULL for stdin layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrStart == NULL){
                            PRINTF_ERROR("TNCore, lyrStart method is NULL for stdin layer.\n");
                            r = FALSE;
                        } else {
                            (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                            //add as last lyr
                            NBArray_addValue(&lstnrs, lstnr);
                        }
                        TNLyrIO_release(&s);
                        TNLyrIO_null(&s);
                    }
                    //connect stack layers
                    if(r && lstnrs.use > 0){
                        //lyrs in array are in inverse order
                        STTNLyrLstnr* sNxt = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, 0);
                        SI32 i; for(i = 1; i < lstnrs.use && r; i++){
                            STTNLyrLstnr* s = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, i);
                            if(s->itf.lyrSetNext != NULL){
                                if(!(*s->itf.lyrSetNext)(sNxt, s->usrParam)){
                                    PRINTF_ERROR("TNCore, lyrSetNext failed for lyr-#%d of %d.\n", (lstnrs.use - i), lstnrs.use);
                                    r = FALSE;
                                } else {
                                    sNxt = s;
                                }
                            }
                        }
                        //keep btm-lyr at conns
                        if(r){
                            //start
                            if(sNxt->itf.lyrStart != NULL){
                                (*sNxt->itf.lyrStart)(sNxt->usrParam);
                            }
                            //retain
                            if(sNxt->itf.lyrRetain != NULL){
                                (*sNxt->itf.lyrRetain)(sNxt->usrParam);
                            }
                            NBArray_addValue(&opq->conns.arr, *sNxt);
                            opq->conns.lastAddTime = NBTimestampMicro_getUTC();
                            PRINTF_INFO("TNCore, added conn with %d-layers (%d conns in total).\n", lstnrs.use, opq->conns.arr.use);
                        }
                    }
                }
            }
            //release
            {
                SI32 i; for(i = 0; i < lstnrs.use; i++){
                    STTNLyrLstnr* s = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, i);
                    if(s->itf.lyrRelease != NULL){
                        (*s->itf.lyrRelease)(s->usrParam);
                    }
                }
                NBArray_empty(&lstnrs);
            }
            NBArray_release(&lstnrs);
        }
        //flags as listening
        if(r){
            opq->isListening = TRUE;
        }
        //revert if failed
        if(!r){
            SI32 i; for(i = 0; i < opq->ports.arr.use; i++){
                STTNCorePortRef p = NBArray_itmValueAtIndex(&opq->ports.arr, STTNCorePortRef, i);
                if(TNCorePort_isSet(p)){
                    TNCorePort_stopFlag(p);
                }
            }
        }
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCore_runAtThisThread(STTNCoreRef ref){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBObject_lock(opq);
    if(opq->isPrepared && opq->isListening && !opq->isRunning && NBIOPollster_isSet(opq->pollster.def) && NBIOPollsterSync_isSet(opq->pollster.sync) && (opq->ports.arr.use > 0 || opq->conns.arr.use > 0)){
        //Running pollsters
        if(!NBIOPollster_engineStart(opq->pollster.def)){
            PRINTF_ERROR("TNCore, NBIOPollster_engineStart failed.\n");
        } else {
            r = TRUE;
            opq->isRunning = TRUE;
            NBObject_unlock(opq);
            //run (unlocked)
            {
                BOOL stopMsgPrinted = FALSE;
                SI64 msAccum = 0, callsPerSecAccum = 0; UI32 secsRunnning = 0;
                STNBTimestampMicro lastSec = NBTimestampMicro_getUTC();
                const SI32 msTimeout = 50;
                BOOL anyFlagActivated = NBStopFlag_isAnyActivated(opq->stopFlag);
                while(opq->conns.arr.use > 0 || (opq->ports.arr.use > 0 && !anyFlagActivated)){
                    BOOL doStop = FALSE;
                    NBASSERT(TNCore_isClass(ref))
                    NBASSERT(TNCore_isClass(TNCore_fromOpqPtr(opq)))
                    //pollster
                    {
                        //Note: poll is required for exit/cleanup process. Do not conditionate.
                        NBIOPollster_enginePoll(opq->pollster.def, msTimeout, opq->pollster.sync);
                        callsPerSecAccum++;
                    }
                    //one-sec-tick
                    {
                        STNBTimestampMicro nowSec = NBTimestampMicro_getUTC();
                        const SI64 ms = NBTimestampMicro_getDiffInMs(&lastSec, &nowSec);
                        if(ms >= 1000){
                            //conns descs
                            /*IF_PRINTF(
                                {
                                    STNBString str;
                                    NBString_initWithSz(&str, 0, 1024, 0.1f);
                                    {
                                        SI32 i; for (i = 0; i < opq->conns.arr.use; i++) {
                                            STTNLyrLstnr s = NBArray_itmValueAtIndex(&opq->conns.arr, STTNLyrLstnr, i);
                                            //stop signal
                                            if (s.itf.lyrConcat != NULL) {
                                                NBString_concat(&str, "#");
                                                NBString_concatSI32(&str, i + 1);
                                                NBString_concat(&str, "/");
                                                NBString_concatSI32(&str, opq->conns.arr.use);
                                                NBString_concat(&str, ":\n");
                                                (*s.itf.lyrConcat)(&str, s.usrParam);
                                                NBString_concat(&str, "\n");
                                            }
                                        }
                                    }
                                    if (str.length > 0) {
                                        PRINTF_INFO("TNCore, conns:\n%s\n", str.str);
                                    }
                                    NBString_release(&str);
                                }
                            );*/
                            PRINTF_INFO("TNCore, %lld polls/sec (%d conns).\n", callsPerSecAccum, opq->conns.arr.use);
                            callsPerSecAccum = 0;
                            msAccum = (msAccum + ms) % 1000;
                            lastSec = nowSec;
                            //
                            secsRunnning++;
                            //autostop
                            if(secsRunnning == opq->args.secsRunAndExit){ //note: zero-value will never trigger this
                                if(!stopMsgPrinted){
                                    PRINTF_INFO("TNCore, stopping by 'secsRunAndExit = %u' rule.\n", opq->args.secsRunAndExit);
                                    stopMsgPrinted = TRUE;
                                }
                                NBStopFlag_activate(opq->stopFlag);
                                doStop = TRUE;
                            }
                        }
                    }
                    //stop
                    {
                        //analyze flag
                        if(!doStop && anyFlagActivated){
                            if(!stopMsgPrinted){
                                PRINTF_INFO("TNCore, stopping by stop-flaged.\n");
                                stopMsgPrinted = TRUE;
                            }
                            NBStopFlag_activate(opq->stopFlag);
                            doStop = TRUE;
                        }
                        //analyze conn-arrival-timout
                        if(!doStop && opq->args.maxSecsWithoutConn > 0 && opq->conns.arr.use == 0){
                            const SI64 msSinceLastConnLeft = NBTimestampMicro_getDiffNowInMs(&opq->conns.lastRemoveTime);
                            if(((SI64)opq->args.maxSecsWithoutConn * 1000) <= msSinceLastConnLeft){
                                if(!stopMsgPrinted){
                                    PRINTF_INFO("TNCore, stopping by 'maxSecsWithoutConn = %u' rule.\n", opq->args.maxSecsWithoutConn);
                                    stopMsgPrinted = TRUE;
                                }
                                NBStopFlag_activate(opq->stopFlag);
                                doStop = TRUE;
                            }
                        }
                        //analyze max-conns-andexit
                        if(!doStop && opq->args.maxConnsAndExit > 0 && opq->args.maxConnsAndExit <= opq->conns.totalArrived && opq->conns.arr.use == 0){
                            if(!stopMsgPrinted){
                                PRINTF_INFO("TNCore, stopping by 'maxConnsAndExit = %u' rule.\n", opq->args.maxConnsAndExit);
                                stopMsgPrinted = TRUE;
                            }
                            NBStopFlag_activate(opq->stopFlag);
                            doStop = TRUE;
                        }
                        //apply
                        if(doStop){
                            SI32 i; for(i = (SI32)opq->conns.arr.use - 1; i >= 0; i--){
                                STTNLyrLstnr s = NBArray_itmValueAtIndex(&opq->conns.arr, STTNLyrLstnr, i);
                                //stop signal
                                if(s.itf.lyrClose != NULL){
                                    (*s.itf.lyrClose)(s.usrParam);
                                }
                            }
                        }
                    }
                    //conns cleanup
                    {
                        SI32 i; for(i = (SI32)opq->conns.arr.use - 1; i >= 0; i--){
                            STTNLyrLstnr s = NBArray_itmValueAtIndex(&opq->conns.arr, STTNLyrLstnr, i);
                            //stop signal
                            if(anyFlagActivated && s.itf.lyrClose != NULL){
                                (*s.itf.lyrClose)(s.usrParam);
                            }
                            //remove
                            if(s.itf.lyrIsRunning != NULL){
                                const BOOL isRunning = (*s.itf.lyrIsRunning)(s.usrParam);
                                if(isRunning){
                                    //trigger actions or cleanup
                                    if(s.itf.lyrConsumeMask != NULL){
                                        (*s.itf.lyrConsumeMask)(NULL /*lnk*/, 0 /*pollMask*/, NULL /*dstPollReq*/, s.usrParam);
                                    }
                                } else {
                                    if(s.itf.lyrRelease != NULL){
                                        (*s.itf.lyrRelease)(s.usrParam);
                                    }
                                    NBArray_removeItemAtIndex(&opq->conns.arr, i);
                                    opq->conns.lastRemoveTime = NBTimestampMicro_getUTC();
                                    PRINTF_INFO("TNCore, conn #%d removed (%d remains).\n", (i + 1), opq->conns.arr.use);
                                }
                            }
                        }
                    }
                    //
                    anyFlagActivated = NBStopFlag_isAnyActivated(opq->stopFlag);
                }
            }
            NBObject_lock(opq);
            opq->isRunning = FALSE;
            NBIOPollster_engineStop(opq->pollster.def);
        }
    }
    NBObject_unlock(opq);
    return r;
}

//

BOOL TNCore_portConnArrivedOpq_(STTNCorePortRef ref, STNBSocketRef socket, const STTNCoreCfgPort* cfg, STNBSslContextRef sslCtx, const STNBX509* sslCAs, const UI32 sslCAsSz, STNBSslContextRef redirSslCtx, const STNBX509* redirSslCAs, const UI32 redirSslCAsSz, void* usrParam){
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)usrParam; NBASSERT(TNCore_isClass(TNCore_fromOpqPtr(usrParam)))
    NBObject_lock(opq);
    if(opq->isPrepared && opq->isListening && !NBStopFlag_isAnyActivated(opq->stopFlag)){
        if(opq->args.maxConnsAndExit > 0 && opq->args.maxConnsAndExit <= opq->conns.totalArrived){
            PRINTF_WARNING("TNCore, ignoring conn (maxConnsAndExit = %u maxed).\n", opq->args.maxConnsAndExit);
        } else if(NBString_strIsEmpty(cfg->redir.server) || cfg->redir.port <= 0){
            PRINTF_WARNING("TNCore, ignoring conn (no redir destination).\n");
        } else {
            STNBArray lstnrs; //STTNLyrLstnr
            NBArray_init(&lstnrs, sizeof(STTNLyrLstnr), NULL);
            r = TRUE;
            //Build layers-stack (in inverse order)
            {
                if(!TNCore_prepareLyrsInvOrderOpq_(opq, cfg, &lstnrs)){
                    r = FALSE;
                } else {
                    //out-socket
                    if(r){
                        STTNLyrLstnr lstnr;
                        STTNLyrIORef s = TNLyrIO_alloc(NULL);
                        NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                        if(!TNLyrIO_setPollsterSync(s, opq->pollster.sync)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setPollsterSync failed for out-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_setParentStopFlag(s, &opq->stopFlag)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setParentStopFlag failed for out-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_getLyrItf(s, &lstnr)){
                            PRINTF_ERROR("TNCore, TNLyrIO_getLyrItf failed for out-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_prepareConnecting(s, cfg->redir.server, cfg->redir.port)){
                            PRINTF_ERROR("TNCore, TNLyrIO_startConnecting failed for out-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRetain == NULL){
                            PRINTF_ERROR("TNCore, lyrRetain method is NULL for out-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRelease == NULL){
                            PRINTF_ERROR("TNCore, lyrRelease method is NULL for out-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrSetNext == NULL){
                            PRINTF_ERROR("TNCore, lyrSetNext method is NULL for out-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrStart == NULL){
                            PRINTF_ERROR("TNCore, lyrStart method is NULL for out-socket layer.\n");
                            r = FALSE;
                        } else {
                            (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                            //add as first lyr
                            NBArray_addItemsAtIndex(&lstnrs, 0, &lstnr, sizeof(lstnr), 1);
                        }
                        TNLyrIO_release(&s);
                        TNLyrIO_null(&s);
                    }
                    //in-socket
                    if(r){
                        STTNLyrLstnr lstnr;
                        STTNLyrIORef s = TNLyrIO_alloc(NULL);
                        NBMemory_setZeroSt(lstnr, STTNLyrLstnr);
                        if(!TNLyrIO_setPollsterSync(s, opq->pollster.sync)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setPollsterSync failed for in-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_setParentStopFlag(s, &opq->stopFlag)){
                            PRINTF_ERROR("TNCore, TNLyrIO_setParentStopFlag failed for in-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_getLyrItf(s, &lstnr)){
                            PRINTF_ERROR("TNCore, TNLyrIO_getLyrItf failed for in-socket layer.\n");
                            r = FALSE;
                        } else if(!TNLyrIO_prepareOwningAcceptedSocket(s, socket)){
                            PRINTF_ERROR("TNCore, TNLyrIO_startConnecting failed for in-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRetain == NULL){
                            PRINTF_ERROR("TNCore, lyrRetain method is NULL for in-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrRelease == NULL){
                            PRINTF_ERROR("TNCore, lyrRelease method is NULL for in-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrSetNext == NULL){
                            PRINTF_ERROR("TNCore, lyrSetNext method is NULL for in-socket layer.\n");
                            r = FALSE;
                        } else if(lstnr.itf.lyrStart == NULL){
                            PRINTF_ERROR("TNCore, lyrStart method is NULL for in-socket layer.\n");
                            r = FALSE;
                        } else {
                            (*lstnr.itf.lyrRetain)(lstnr.usrParam);
                            //add as last lyr
                            NBArray_addValue(&lstnrs, lstnr);
                        }
                        TNLyrIO_release(&s);
                        TNLyrIO_null(&s);
                    }
                }
                //connect stack layers
                if(r && lstnrs.use > 0){
                    //lyrs in array are in inverse order
                    STTNLyrLstnr* sNxt = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, 0);
                    SI32 i; for(i = 1; i < lstnrs.use && r; i++){
                        STTNLyrLstnr* s = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, i);
                        if(s->itf.lyrSetNext != NULL){
                            if(!(*s->itf.lyrSetNext)(sNxt, s->usrParam)){
                                PRINTF_ERROR("TNCore, lyrSetNext failed for lyr-#%d of %d.\n", (lstnrs.use - i), lstnrs.use);
                                r = FALSE;
                            } else {
                                sNxt = s;
                            }
                        }
                    }
                    //keep btm-lyr at conns
                    if(r){
                        //start
                        if(sNxt->itf.lyrStart != NULL){
                            (*sNxt->itf.lyrStart)(sNxt->usrParam);
                        }
                        //retain
                        if(sNxt->itf.lyrRetain != NULL){
                            (*sNxt->itf.lyrRetain)(sNxt->usrParam);
                        }
                        NBArray_addValue(&opq->conns.arr, *sNxt);
                        opq->conns.lastAddTime = NBTimestampMicro_getUTC();
                        PRINTF_INFO("TNCore, added conn with %d-layers (%d conns in total).\n", lstnrs.use, opq->conns.arr.use);
                    }
                }
            }
            //release
            {
                SI32 i; for(i = 0; i < lstnrs.use; i++){
                    STTNLyrLstnr* s = NBArray_itmPtrAtIndex(&lstnrs, STTNLyrLstnr, i);
                    if(s->itf.lyrRelease != NULL){
                        (*s->itf.lyrRelease)(s->usrParam);
                    }
                }
                NBArray_empty(&lstnrs);
            }
            NBArray_release(&lstnrs);
        }
        //
        opq->conns.totalArrived++;
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCore_isRunning(STTNCoreRef ref) {
    BOOL r = FALSE;
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBASSERT(opq != NULL)
    //NBObject_lock(opq); //no need for lock.
    {
        r = opq->isRunning;
    }
    //NBObject_unlock(opq); //no need for lock.
    return r;
}

void TNCore_stopFlag(STTNCoreRef ref) {
    STTNCoreOpq* opq = (STTNCoreOpq*)ref.opaque; NBASSERT(TNCore_isClass(ref))
    NBASSERT(opq != NULL)
    NBObject_lock(opq);
    {
        NBStopFlag_activate(opq->stopFlag);
    }
    NBObject_unlock(opq);
}
