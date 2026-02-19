//
//  TNCorePort.c
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
#include "nb/crypto/NBBase64.h"
#include "nb/crypto/NBX509.h"
#include "nb/crypto/NBPkcs12.h"
#include "nb/ssl/NBSslContext.h"
#include "core/TNCorePort.h"

//TNCorePortLstnr

typedef struct STTNCorePortLstnr_ {
    STTNCorePortLstnrItf    itf;
    void*                   usrParam;
} STTNCorePortLstnr;

//TNCorePortOpq

typedef struct STTNCorePortOpq_ {
    STNBObject          prnt;
    STNBThreadCond      cond;
    STNBStopFlagRef     stopFlag;
    STTNCoreCfgPort     cfg;
    STTNCorePortLstnr   lstnr;
    //pollster
    struct {
        STNBIOPollsterSyncRef   sync; //default
        STNBIOPollstersProviderRef provider; //provider
        BOOL        isListening;
    } pollster;
    //net
    struct {
        STNBSocketRef   socket;
        BOOL            isBinded;
        //ssl
        struct {
            STNBSslContextRef   context;
            STNBString          name;    //friendly name
            //CAs
            struct {
                STNBArray   arr;        //STNBX509
            } CAs;
        } ssl;
    } net;
    //redir
    struct {
        //ssl
        struct {
            STNBSslContextRef   context;
            STNBString          name;    //friendly name
            //CAs
            struct {
                STNBArray       arr;        //STNBX509
            } CAs;
        } ssl;
    } redir;
} STTNCorePortOpq;

//pollster callbacks

void TNCorePort_pollConsumeMask_(STNBIOLnk ioLnk, const UI8 pollMask, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData);
void TNCorePort_pollConsumeNoOp_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData);
void TNCorePort_pollRemoved_(STNBIOLnk ioLnk, void* usrData);

//

NB_OBJREF_BODY(TNCorePort, STTNCorePortOpq, NBObject)

void TNCorePort_initZeroed(STNBObject* obj){
    STTNCorePortOpq* opq    = (STTNCorePortOpq*)obj;
    NBThreadCond_init(&opq->cond);
    opq->stopFlag           = NBStopFlag_alloc(NULL);
    //pollster
    {
        //
    }
    //net
    {
        //ssl
        {
            opq->net.ssl.context = NB_OBJREF_NULL;
            NBString_init(&opq->net.ssl.name); //friendly name
            //CAs
            {
                NBArray_init(&opq->net.ssl.CAs.arr, sizeof(STNBX509), NULL);
            }
        }
    }
    //redir
    {
        //ssl
        {
            opq->redir.ssl.context = NB_OBJREF_NULL;
            NBString_init(&opq->redir.ssl.name); //friendly name
            //CAs
            {
                NBArray_init(&opq->redir.ssl.CAs.arr, sizeof(STNBX509), NULL);
            }
        }
    }
}

void TNCorePort_uninitLocked(STNBObject* obj){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)obj;
    //
    if(NBStopFlag_isSet(opq->stopFlag)){
        NBStopFlag_activate(opq->stopFlag);
    }
    //pollster
    {
        //wait for socket removal from pollster
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
    //net
    {
        //ssl
        {
            //CAs
            {
                SI32 i; for(i = 0; i < opq->net.ssl.CAs.arr.use; i++){
                    STNBX509* c = NBArray_itmPtrAtIndex(&opq->net.ssl.CAs.arr, STNBX509, i);
                    NBX509_release(c);
                }
                NBArray_empty(&opq->net.ssl.CAs.arr);
                NBArray_release(&opq->net.ssl.CAs.arr);
            }
            if(NBSslContext_isSet(opq->net.ssl.context)){
                NBSslContext_release(&opq->net.ssl.context);
                NBSslContext_null(&opq->net.ssl.context);
            }
            NBString_release(&opq->net.ssl.name); //friendly name
        }
        if(NBSocket_isSet(opq->net.socket)){
            NBSocket_release(&opq->net.socket);
            NBSocket_null(&opq->net.socket);
        }
        opq->net.isBinded = FALSE;
    }
    //redir
    {
        //ssl
        {
            //CAs
            {
                SI32 i; for(i = 0; i < opq->redir.ssl.CAs.arr.use; i++){
                    STNBX509* c = NBArray_itmPtrAtIndex(&opq->redir.ssl.CAs.arr, STNBX509, i);
                    NBX509_release(c);
                }
                NBArray_empty(&opq->redir.ssl.CAs.arr);
                NBArray_release(&opq->redir.ssl.CAs.arr);
            }
            if(NBSslContext_isSet(opq->redir.ssl.context)){
                NBSslContext_release(&opq->redir.ssl.context);
                NBSslContext_null(&opq->redir.ssl.context);
            }
            NBString_release(&opq->redir.ssl.name); //friendly name
        }
    }
    //
    if(NBStopFlag_isSet(opq->stopFlag)){
        NBStopFlag_release(&opq->stopFlag);
        NBStopFlag_null(&opq->stopFlag);
    }
    //cfg
    NBStruct_stRelease(TNCoreCfgPort_getSharedStructMap(), &opq->cfg, sizeof(opq->cfg));
    NBThreadCond_release(&opq->cond);
}

BOOL TNCorePort_setPollster(STTNCorePortRef ref, STNBIOPollsterSyncRef pollSync){    //when one pollster only
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
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

BOOL TNCorePort_setPollstersProvider(STTNCorePortRef ref, STNBIOPollstersProviderRef provider){ //when multiple pollsters
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
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

BOOL TNCorePort_setParentStopFlag(STTNCorePortRef ref, STNBStopFlagRef* parentStopFlag){
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
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

BOOL TNCorePort_setListener(STTNCorePortRef ref, STTNCorePortLstnrItf* itf, void* usrParam){
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    NBObject_lock(opq);
    if(!opq->pollster.isListening && !NBStopFlag_isMineActivated(opq->stopFlag) && !opq->net.isBinded){
        if(itf == NULL){
            NBMemory_setZeroSt(opq->lstnr, STTNCorePortLstnr);
        } else {
            opq->lstnr.itf = *itf;
            opq->lstnr.usrParam = usrParam;
        }
        r = TRUE;
    }
    NBObject_unlock(opq);
    return r;
}


//

UI32 TNCorePort_getPort(STTNCorePortRef ref){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    return opq->cfg.port;
}

BOOL TNCorePort_prepare(STTNCorePortRef ref, const STTNCoreCfgPort* cfg, STNBX509* globalCAs, const UI32 globalCAsSz){
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    NBObject_lock(opq);
    if(!opq->pollster.isListening && !NBStopFlag_isMineActivated(opq->stopFlag) && !opq->net.isBinded && cfg != NULL && cfg->port > 0){
        r = TRUE;
        //ssl
        if(r){
            BOOL hasSslLayer = FALSE;
            if(cfg->layers != NULL && cfg->layersSz > 0){
                UI32 i; for(i = 0; i < cfg->layersSz; i++){
                    if(NBString_strIsLike(cfg->layers[i], "ssl")){
                        hasSslLayer = TRUE;
                        break;
                    }
                }
            }
            if(r && hasSslLayer){
                //load CAs
                if(r){
                    STNBArray arr;
                    NBArray_init(&arr, sizeof(STNBX509), NULL);
                    //load CAs
                    {
                        UI32 i; for(i = 0; i < cfg->ssl.casSz && r; i++){
                            STTNCoreCfgSslCertSrc* src = &cfg->ssl.cas[i];
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for root CA-cert.\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c;
                                    NBX509_init(&c);
                                    if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBX509_createFromDERBytes failed for root CA-cert.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBX509_createFromDERBytes success (base64).\n");
                                        NBArray_addValue(&arr, c);
                                    }
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBFileRef f = NBFile_alloc(NULL);
                                if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                                    PRINTF_ERROR("TNCorePort, NBFile_open failed for root CA-cert: '%s'.\n", src->path);
                                    r = FALSE;
                                } else {
                                    NBFile_lock(f);
                                    {
                                        STNBX509 c;
                                        NBX509_init(&c);
                                        if(!NBX509_createFromDERFile(&c, f)){
                                            PRINTF_ERROR("TNCorePort, NBX509_createFromDERFile failed for root CA-cert: '%s'.\n", src->path);
                                            r = FALSE;
                                            NBX509_release(&c);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBX509_createFromDERFile success (file-path).\n");
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
                            SI32 i; for(i = 0; i < opq->net.ssl.CAs.arr.use; i++){
                                STNBX509* c = NBArray_itmPtrAtIndex(&opq->net.ssl.CAs.arr, STNBX509, i);
                                NBX509_release(c);
                            }
                            NBArray_empty(&opq->net.ssl.CAs.arr);
                        }
                        //move from newArr to current
                        {
                            SI32 i; for(i = 0; i < arr.use; i++){
                                STNBX509* c = NBArray_itmPtrAtIndex(&arr, STNBX509, i);
                                NBArray_addValue(&opq->net.ssl.CAs.arr, *c);
                            }
                            NBArray_empty(&arr);
                        }
                        //add global CAs
                        if(globalCAs != NULL && globalCAsSz > 0){
                            UI32 i; for(i = 0; i < globalCAsSz; i++){
                                STNBX509 c;
                                NBX509_init(&c);
                                if(!NBX509_createFromOther(&c, &globalCAs[i])){
                                    PRINTF_ERROR("TNCorePort, NBX509_createFromOther failed for root flobal-CA-cert(%u).\n", i);
                                    r = FALSE;
                                    NBX509_release(&c);
                                } else {
                                    NBArray_addValue(&opq->net.ssl.CAs.arr, c);
                                }
                            }
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
                //net, ssl
                if(r){
                    STNBSslContextRef ctx = NBSslContext_alloc(NULL);
                    if(!NBSslContext_create(ctx, NBSslContext_getServerMode)){
                        PRINTF_ERROR("TNCorePort, NBSslContext_create for port.\n", cfg->port);
                        r = FALSE;
                    } else {
                        STNBX509 cert; STNBPKey key;
                        NBX509_init(&cert);
                        NBPKey_init(&key);
                        //load cert
                        if(r){
                            const STTNCoreCfgSslCertSrc* src = &cfg->ssl.cert.source;
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for root cert.\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c;
                                    NBX509_init(&c);
                                    if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBX509_createFromDERBytes failed for root cert.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBX509_createFromDERBytes success (base64).\n");
                                        NBX509_release(&cert);
                                        cert = c;
                                    }
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBFileRef f = NBFile_alloc(NULL);
                                if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                                    PRINTF_ERROR("TNCorePort, NBFile_open failed for root cert: '%s'.\n", src->path);
                                    r = FALSE;
                                } else {
                                    NBFile_lock(f);
                                    {
                                        STNBX509 c;
                                        NBX509_init(&c);
                                        if(!NBX509_createFromDERFile(&c, f)){
                                            PRINTF_ERROR("TNCorePort, NBX509_createFromDERFile failed for root cert: '%s'.\n", src->path);
                                            r = FALSE;
                                            NBX509_release(&c);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBX509_createFromDERFile success (file-path).\n");
                                            NBX509_release(&cert);
                                            cert = c;
                                        }
                                    }
                                    NBFile_unlock(f);
                                }
                                NBFile_release(&f);
                            }
                        }
                        //load pkey
                        if(r){
                            const STNTNCoreCfgSslKey* src = &cfg->ssl.cert.source.key;
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for key.\n");
                                    r = FALSE;
                                } else {
                                    STNBPkcs12 p;
                                    NBPkcs12_init(&p);
                                    if(!NBPkcs12_createFromDERBytes(&p, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBPkcs12_createFromDERBytes failed for key.\n");
                                        r = FALSE;
                                    } else {
                                        STNBX509 c; STNBPKey k;
                                        NBX509_init(&c);
                                        NBPKey_init(&k);
                                        if(!NBPkcs12_getCertAndKey(&p, &k, &c, src->pass)){
                                            PRINTF_ERROR("TNCorePort, NBPkcs12_getCertAndKey failed for key.\n");
                                            r = FALSE;
                                            NBX509_release(&c);
                                            NBPKey_release(&k);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBPkcs12_getCertAndKey success.\n");
                                            if(NBX509_isCreated(&c)){
                                                NBX509_release(&cert);
                                                cert = c;
                                            } else {
                                                NBX509_release(&c);
                                            }
                                            if(NBPKey_isCreated(&k)){
                                                NBPKey_release(&key);
                                                key = k;
                                            } else {
                                                NBPKey_release(&k);
                                            }
                                        }
                                    }
                                    NBPkcs12_release(&p);
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBPkcs12 p;
                                NBPkcs12_init(&p);
                                if(!NBPkcs12_createFromDERFile(&p, src->path)){
                                    PRINTF_ERROR("TNCorePort, NBPkcs12_createFromDERBytes failed for key..\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c; STNBPKey k;
                                    NBX509_init(&c);
                                    NBPKey_init(&k);
                                    if(!NBPkcs12_getCertAndKey(&p, &k, &c, src->pass)){
                                        PRINTF_ERROR("TNCorePort, NBPkcs12_getCertAndKey failed for key.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                        NBPKey_release(&k);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBPkcs12_getCertAndKey success.\n");
                                        if(NBX509_isCreated(&c)){
                                            NBX509_release(&cert);
                                            cert = c;
                                        } else {
                                            NBX509_release(&c);
                                        }
                                        if(NBPKey_isCreated(&k)){
                                            NBPKey_release(&key);
                                            key = k;
                                        } else {
                                            NBPKey_release(&k);
                                        }
                                    }
                                }
                                NBPkcs12_release(&p);
                            }
                        }
                        //apply
                        if(r){
                            if(NBPKey_isCreated(&key) && NBX509_isCreated(&cert)){
                                if(!NBSslContext_attachCertAndkey(ctx, &cert, &key)){
                                    PRINTF_ERROR("TNCorePort, NBSslContext_attachCertAndkey failed.\n");
                                    r = FALSE;
                                } else {
                                    //PRINTF_INFO("TNCorePort, NBSslContext_attachCertAndkey success.\n");
                                }
                            } else if(NBPKey_isCreated(&key)){
                                PRINTF_ERROR("TNCorePort, got pkey but missing cert for sslContext.\n");
                                r = FALSE;
                            } else if(NBX509_isCreated(&cert)){
                                PRINTF_ERROR("TNCorePort, got ert but missing pkey for sslContext.\n");
                                r = FALSE;
                            }
                        }
                        NBX509_release(&cert);
                        NBPKey_release(&key);
                    }
                    //add CAs
                    if(r){
                        SI32 i; for(i = 0; i < opq->net.ssl.CAs.arr.use && r; i++){
                            STNBX509* c = NBArray_itmPtrAtIndex(&opq->net.ssl.CAs.arr, STNBX509, i);
                            if(!NBSslContext_addCAToStore(ctx, c)){
                                PRINTF_ERROR("TNCorePort, NBSslContext_addCAToStore failed for port: %u.\n", cfg->port);
                                r = FALSE;
                            } else if(cfg->ssl.cert.isRequested && !NBSslContext_addCAToRequestList(ctx, c)){
                                PRINTF_ERROR("TNCorePort, NBSslContext_addCAToRequestList failed for port: %u.\n", cfg->port);
                                r = FALSE;
                            }
                        }
                    }
                    //apply
                    if(r){
                        if(!NBSslContext_setVerifyPeerCert(ctx, cfg->ssl.cert.isRequested, cfg->ssl.cert.isRequired)){
                            PRINTF_ERROR("TNCorePort, NBSslContext_setVerifyPeerCert failed for port: %u.\n", cfg->port);
                            r = FALSE;
                        } else {
                            NBSslContext_set(&opq->net.ssl.context, &ctx);
                            PRINTF_INFO("TNCorePort, sslContext applied to in-port: %u.\n", cfg->port);
                        }
                    }
                    NBSslContext_release(&ctx);
                    NBSslContext_null(&ctx);
                }
            }
        }
        //redir ssl
        if(r){
            BOOL hasSslLayer = FALSE;
            if(cfg->redir.layers != NULL && cfg->redir.layersSz > 0){
                UI32 i; for(i = 0; i < cfg->redir.layersSz; i++){
                    if(NBString_strIsLike(cfg->redir.layers[i], "ssl")){
                        hasSslLayer = TRUE;
                        break;
                    }
                }
            }
            if(r && hasSslLayer){
                //redir, load CAs
                if(r){
                    STNBArray arr;
                    NBArray_init(&arr, sizeof(STNBX509), NULL);
                    //load CAs
                    {
                        UI32 i; for(i = 0; i < cfg->redir.ssl.casSz && r; i++){
                            STTNCoreCfgSslCertSrc* src = &cfg->redir.ssl.cas[i];
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for root CA-cert.\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c;
                                    NBX509_init(&c);
                                    if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBX509_createFromDERBytes failed for root CA-cert.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBX509_createFromDERBytes success (base64).\n");
                                        NBArray_addValue(&arr, c);
                                    }
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBFileRef f = NBFile_alloc(NULL);
                                if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                                    PRINTF_ERROR("TNCorePort, NBFile_open failed for root CA-cert: '%s'.\n", src->path);
                                    r = FALSE;
                                } else {
                                    NBFile_lock(f);
                                    {
                                        STNBX509 c;
                                        NBX509_init(&c);
                                        if(!NBX509_createFromDERFile(&c, f)){
                                            PRINTF_ERROR("TNCorePort, NBX509_createFromDERFile failed for root CA-cert: '%s'.\n", src->path);
                                            r = FALSE;
                                            NBX509_release(&c);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBX509_createFromDERFile success (file-path).\n");
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
                            SI32 i; for(i = 0; i < opq->redir.ssl.CAs.arr.use; i++){
                                STNBX509* c = NBArray_itmPtrAtIndex(&opq->redir.ssl.CAs.arr, STNBX509, i);
                                NBX509_release(c);
                            }
                            NBArray_empty(&opq->redir.ssl.CAs.arr);
                        }
                        //move from newArr to current
                        {
                            SI32 i; for(i = 0; i < arr.use; i++){
                                STNBX509* c = NBArray_itmPtrAtIndex(&arr, STNBX509, i);
                                NBArray_addValue(&opq->redir.ssl.CAs.arr, *c);
                            }
                            NBArray_empty(&arr);
                        }
                        //add global CAs
                        if(globalCAs != NULL && globalCAsSz > 0){
                            UI32 i; for(i = 0; i < globalCAsSz; i++){
                                STNBX509 c;
                                NBX509_init(&c);
                                if(!NBX509_createFromOther(&c, &globalCAs[i])){
                                    PRINTF_ERROR("TNCorePort, NBX509_createFromOther failed for root flobal-CA-cert(%u).\n", i);
                                    r = FALSE;
                                    NBX509_release(&c);
                                } else {
                                    NBArray_addValue(&opq->redir.ssl.CAs.arr, c);
                                }
                            }
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
                //redir, ssl
                if(r){
                    STNBSslContextRef ctx = NBSslContext_alloc(NULL);
                    if(!NBSslContext_create(ctx, NBSslContext_getServerMode)){
                        PRINTF_ERROR("TNCorePort, NBSslContext_create for port.\n", cfg->port);
                        r = FALSE;
                    } else {
                        STNBX509 cert; STNBPKey key;
                        NBX509_init(&cert);
                        NBPKey_init(&key);
                        //load cert
                        if(r){
                            const STTNCoreCfgSslCertSrc* src = &cfg->redir.ssl.cert.source;
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for root cert.\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c;
                                    NBX509_init(&c);
                                    if(!NBX509_createFromDERBytes(&c, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBX509_createFromDERBytes failed for root cert.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBX509_createFromDERBytes success (base64).\n");
                                        NBX509_release(&cert);
                                        cert = c;
                                    }
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBFileRef f = NBFile_alloc(NULL);
                                if(!NBFile_open(f, src->path, ENNBFileMode_Read)){
                                    PRINTF_ERROR("TNCorePort, NBFile_open failed for root cert: '%s'.\n", src->path);
                                    r = FALSE;
                                } else {
                                    NBFile_lock(f);
                                    {
                                        STNBX509 c;
                                        NBX509_init(&c);
                                        if(!NBX509_createFromDERFile(&c, f)){
                                            PRINTF_ERROR("TNCorePort, NBX509_createFromDERFile failed for root cert: '%s'.\n", src->path);
                                            r = FALSE;
                                            NBX509_release(&c);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBX509_createFromDERFile success (file-path).\n");
                                            NBX509_release(&cert);
                                            cert = c;
                                        }
                                    }
                                    NBFile_unlock(f);
                                }
                                NBFile_release(&f);
                            }
                        }
                        //load pkey
                        if(r){
                            const STNTNCoreCfgSslKey* src = &cfg->redir.ssl.cert.source.key;
                            if(!NBString_strIsEmpty(src->pay64)){
                                STNBString pay;
                                NBString_init(&pay);
                                if(!NBBase64_decode(&pay, src->pay64)){
                                    PRINTF_ERROR("TNCorePort, NBBase64_decode failed for key.\n");
                                    r = FALSE;
                                } else {
                                    STNBPkcs12 p;
                                    NBPkcs12_init(&p);
                                    if(!NBPkcs12_createFromDERBytes(&p, pay.str, pay.length)){
                                        PRINTF_ERROR("TNCorePort, NBPkcs12_createFromDERBytes failed for key.\n");
                                        r = FALSE;
                                    } else {
                                        STNBX509 c; STNBPKey k;
                                        NBX509_init(&c);
                                        NBPKey_init(&k);
                                        if(!NBPkcs12_getCertAndKey(&p, &k, &c, src->pass)){
                                            PRINTF_ERROR("TNCorePort, NBPkcs12_getCertAndKey failed for key.\n");
                                            r = FALSE;
                                            NBX509_release(&c);
                                            NBPKey_release(&k);
                                        } else {
                                            //PRINTF_INFO("TNCorePort, NBPkcs12_getCertAndKey success.\n");
                                            if(NBX509_isCreated(&c)){
                                                NBX509_release(&cert);
                                                cert = c;
                                            } else {
                                                NBX509_release(&c);
                                            }
                                            if(NBPKey_isCreated(&k)){
                                                NBPKey_release(&key);
                                                key = k;
                                            } else {
                                                NBPKey_release(&k);
                                            }
                                        }
                                    }
                                    NBPkcs12_release(&p);
                                }
                                NBString_release(&pay);
                            } else if(!NBString_strIsEmpty(src->path)){
                                STNBPkcs12 p;
                                NBPkcs12_init(&p);
                                if(!NBPkcs12_createFromDERFile(&p, src->path)){
                                    PRINTF_ERROR("TNCorePort, NBPkcs12_createFromDERBytes failed for key..\n");
                                    r = FALSE;
                                } else {
                                    STNBX509 c; STNBPKey k;
                                    NBX509_init(&c);
                                    NBPKey_init(&k);
                                    if(!NBPkcs12_getCertAndKey(&p, &k, &c, src->pass)){
                                        PRINTF_ERROR("TNCorePort, NBPkcs12_getCertAndKey failed for key.\n");
                                        r = FALSE;
                                        NBX509_release(&c);
                                        NBPKey_release(&k);
                                    } else {
                                        //PRINTF_INFO("TNCorePort, NBPkcs12_getCertAndKey success.\n");
                                        if(NBX509_isCreated(&c)){
                                            NBX509_release(&cert);
                                            cert = c;
                                        } else {
                                            NBX509_release(&c);
                                        }
                                        if(NBPKey_isCreated(&k)){
                                            NBPKey_release(&key);
                                            key = k;
                                        } else {
                                            NBPKey_release(&k);
                                        }
                                    }
                                }
                                NBPkcs12_release(&p);
                            }
                        }
                        //apply
                        if(r){
                            if(NBPKey_isCreated(&key) && NBX509_isCreated(&cert)){
                                if(!NBSslContext_attachCertAndkey(ctx, &cert, &key)){
                                    PRINTF_ERROR("TNCorePort, NBSslContext_attachCertAndkey failed.\n");
                                    r = FALSE;
                                } else {
                                    //PRINTF_INFO("TNCorePort, NBSslContext_attachCertAndkey success.\n");
                                }
                            } else if(NBPKey_isCreated(&key)){
                                PRINTF_ERROR("TNCorePort, got pkey but missing cert for sslContext.\n");
                                r = FALSE;
                            } else if(NBX509_isCreated(&cert)){
                                PRINTF_ERROR("TNCorePort, got ert but missing pkey for sslContext.\n");
                                r = FALSE;
                            }
                        }
                        NBX509_release(&cert);
                        NBPKey_release(&key);
                    }
                    //add CAs
                    if(r){
                        SI32 i; for(i = 0; i < opq->redir.ssl.CAs.arr.use && r; i++){
                            STNBX509* c = NBArray_itmPtrAtIndex(&opq->redir.ssl.CAs.arr, STNBX509, i);
                            if(!NBSslContext_addCAToStore(ctx, c)){
                                PRINTF_ERROR("TNCorePort, NBSslContext_addCAToStore failed for port: %u.\n", cfg->port);
                                r = FALSE;
                            } else if(cfg->redir.ssl.cert.isRequested && !NBSslContext_addCAToRequestList(ctx, c)){
                                PRINTF_ERROR("TNCorePort, NBSslContext_addCAToRequestList failed for port: %u.\n", cfg->port);
                                r = FALSE;
                            }
                        }
                    }
                    //apply
                    if(r){
                        if(!NBSslContext_setVerifyPeerCert(ctx, cfg->redir.ssl.cert.isRequested, cfg->redir.ssl.cert.isRequired)){
                            PRINTF_ERROR("TNCorePort, NBSslContext_setVerifyPeerCert failed for port: %u.\n", cfg->port);
                            r = FALSE;
                        } else {
                            NBSslContext_set(&opq->redir.ssl.context, &ctx);
                            PRINTF_INFO("TNCorePort, sslContext applied to out-port: %u.\n", cfg->port);
                        }
                    }
                    NBSslContext_release(&ctx);
                    NBSslContext_null(&ctx);
                }
            }
        }
        //bind
        if(r){
            STNBSocketRef s = NBSocket_alloc(NULL);
            //
            NBSocket_setNoSIGPIPE(s, TRUE);
            NBSocket_setCorkEnabled(s, FALSE);
            NBSocket_setDelayEnabled(s, FALSE);
            NBSocket_setNonBlocking(s, TRUE);
            NBSocket_setUnsafeMode(s, TRUE);
            //
            NBSocket_setReuseAddr(s, TRUE);
            NBSocket_setReusePort(s, TRUE);
            //
            if(!NBSocket_bind(s, cfg->port)){
                PRINTF_ERROR("TNCorePort, NBSocket_bind(%u) failed.\n", cfg->port);
                r = FALSE;
            } else {
                NBSocket_set(&opq->net.socket, &s);
                opq->net.isBinded = TRUE;
            }
            NBSocket_release(&s);
            NBSocket_null(&s);
        }
        //cfg
        if(r){
            NBStruct_stRelease(TNCoreCfgPort_getSharedStructMap(), &opq->cfg, sizeof(opq->cfg));
            NBStruct_stClone(TNCoreCfgPort_getSharedStructMap(), cfg, sizeof(*cfg), &opq->cfg, sizeof(opq->cfg));
        }
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCorePort_startListening(STTNCorePortRef ref){
    BOOL r = FALSE;
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    NBObject_lock(opq);
    if(!NBStopFlag_isMineActivated(opq->stopFlag) && opq->cfg.port > 0 && opq->net.isBinded && !opq->pollster.isListening){
        if(!NBSocket_listen(opq->net.socket)){
            PRINTF_ERROR("TNCorePort, NBSocket_listen failed.\n");
        } else {
            //pollster
            {
                STNBIOPollsterSyncRef pollSync = opq->pollster.sync;
                STNBIOPollsterLstrnItf itf;
                //itf
                {
                    NBMemory_setZeroSt(itf, STNBIOPollsterLstrnItf);
                    itf.pollConsumeMask = TNCorePort_pollConsumeMask_;
                    itf.pollConsumeNoOp = TNCorePort_pollConsumeNoOp_;
                    itf.pollRemoved     = TNCorePort_pollRemoved_;
                }
                //get a pollster from provider
                if(!NBIOPollsterSync_isSet(pollSync) && NBIOPollstersProvider_isSet(opq->pollster.provider)){
                    pollSync =  NBIOPollstersProvider_getPollsterSync(opq->pollster.provider);
                }
                //create default pollsterSync
                if(!NBIOPollsterSync_isSet(pollSync)){
                    pollSync = opq->pollster.sync = NBIOPollsterSync_alloc(NULL);
                }
                //add to pollSync
                if(!opq->pollster.isListening && NBIOPollsterSync_isSet(pollSync)){
                    opq->pollster.isListening = TRUE; //set flag first in case callback is called
                    if(!NBIOPollsterSync_addSocketWithItf(pollSync, opq->net.socket, ENNBIOPollsterOpBit_Read, &itf, opq)){
                        opq->pollster.isListening = FALSE;
                    }
                }
                //result
                if(opq->pollster.isListening){
                    r = TRUE;
                }
            }
        }
    }
    NBObject_unlock(opq);
    return r;
}

BOOL TNCorePort_isBusy(STTNCorePortRef ref){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    return opq->pollster.isListening;
}
    
void TNCorePort_stopFlag(STTNCorePortRef ref){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)ref.opaque; NBASSERT(TNCorePort_isClass(ref))
    NBStopFlag_activate(opq->stopFlag);
}

//pollster callbacks

void TNCorePort_pollConsumeMask_(STNBIOLnk ioLnk, const UI8 pollMask, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)usrData; NBASSERT(TNCorePort_isClass(NBObjRef_fromOpqPtr(opq)))
    IF_NBASSERT(STNBTimestampMicro timeLastAction = NBTimestampMicro_getUTC(), timeCur; SI64 usDiff;)
    //read
    if(pollMask & ENNBIOPollsterOpBit_Read){
        //receive clients
        STNBSocketRef cSckt = NBSocket_alloc(NULL);
        while(NBSocket_accept(opq->net.socket, cSckt)) {
            //notify
            if(opq->lstnr.itf.portConnArrived != NULL){
                (*opq->lstnr.itf.portConnArrived)(TNCorePort_fromOpqPtr(opq), cSckt, &opq->cfg, opq->net.ssl.context, NBArray_dataPtr(&opq->net.ssl.CAs.arr, STNBX509), (UI32)opq->net.ssl.CAs.arr.use, opq->redir.ssl.context, NBArray_dataPtr(&opq->redir.ssl.CAs.arr, STNBX509), (UI32)opq->redir.ssl.CAs.arr.use, opq->lstnr.usrParam);
            }
            //reset socket
            {
                NBSocket_release(&cSckt);
                NBSocket_null(&cSckt);
                cSckt = NBSocket_alloc(NULL);
            }
        }
        NBSocket_release(&cSckt);
        NBSocket_null(&cSckt);
    }
    //error
    if(pollMask & ENNBIOPollsterOpBits_ErrOrGone){
        PRINTF_INFO("TNCorePort, port err-or-gone.\n");
        NBStopFlag_activate(opq->stopFlag);
    }
    //consume pend tasks
    /*if(NB_RTP_CLIENT_PORT_HAS_PEND_TASK(p)){
        NBRtpClient_portConsumePendTasksOpq_(opq, p);
    }*/
    //return
    dstUpd->opsMasks = ENNBIOPollsterOpBit_Read;
    //consume stopFlag
    if(NBStopFlag_isAnyActivated(opq->stopFlag) && NBIOPollsterSync_isSet(dstSync)){
        NBASSERT(opq->pollster.isListening)
        NBIOPollsterSync_removeIOLnk(dstSync, &ioLnk);
    }
    //
#    ifdef NB_CONFIG_INCLUDE_ASSERTS
    {
        timeCur   = NBTimestampMicro_getUTC();
        usDiff    = NBTimestampMicro_getDiffInUs(&timeLastAction, &timeCur);
        /*if(usDiff >= 1000ULL){
            PRINTF_INFO("TNCorePort, pollConsumeMask(%s%s%s) took %llu.%llu%llums.\n", (pollMask & ENNBIOPollsterOpBit_Read ? "+read" : ""), (pollMask & ENNBIOPollsterOpBit_Write ? "+write" : ""), (pollMask & ENNBIOPollsterOpBits_ErrOrGone ? "+errOrGone" : ""), (usDiff / 1000ULL), (usDiff % 1000ULL) % 100ULL, (usDiff % 100ULL) % 10ULL);
        }*/
        timeLastAction = timeCur;
    }
#    endif
}

void TNCorePort_pollConsumeNoOp_(STNBIOLnk ioLnk, STNBIOPollsterUpd* dstUpd, STNBIOPollsterSyncRef dstSync, void* usrData){
    NBASSERT(TNCorePort_isClass(NBObjRef_fromOpqPtr((STTNCorePortOpq*)usrData)))
    TNCorePort_pollConsumeMask_(ioLnk, 0, dstUpd, dstSync, usrData);
}

void TNCorePort_pollRemoved_(STNBIOLnk ioLnk, void* usrData){
    STTNCorePortOpq* opq = (STTNCorePortOpq*)usrData; NBASSERT(TNCorePort_isClass(NBObjRef_fromOpqPtr(opq)))
    opq->pollster.isListening = FALSE;
}
