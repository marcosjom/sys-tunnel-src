//
//  TNCoreCfg.c
//  sys-tunnel-core-osx
//
//  Created by Marcos Ortega on 15/3/23.
//

#include "nb/NBFrameworkPch.h"
#include "nb/core/NBStruct.h"
#include "nb/core/NBMngrStructMaps.h"
//
#include "core/TNCoreCfg.h"

//NTNCoreCfgSslKey

STNBStructMapsRec NTNCoreCfgSslKey_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* NTNCoreCfgSslKey_getSharedStructMap(void){
    NBMngrStructMaps_lock(&NTNCoreCfgSslKey_sharedStructMap);
    if(NTNCoreCfgSslKey_sharedStructMap.map == NULL){
        STNTNCoreCfgSslKey s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STNTNCoreCfgSslKey);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addStrPtrM(map, s, pass);
        NBStructMap_addStrPtrM(map, s, path);
        NBStructMap_addStrPtrM(map, s, pay64);
        NBStructMap_addStrPtrM(map, s, name);
        NTNCoreCfgSslKey_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&NTNCoreCfgSslKey_sharedStructMap);
    return NTNCoreCfgSslKey_sharedStructMap.map;
}

//TNCoreCfgSslCertSrc

STNBStructMapsRec TNCoreCfgSslCertSrc_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgSslCertSrc_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgSslCertSrc_sharedStructMap);
    if(TNCoreCfgSslCertSrc_sharedStructMap.map == NULL){
        STTNCoreCfgSslCertSrc s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgSslCertSrc);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addStrPtrM(map, s, path);
        NBStructMap_addStrPtrM(map, s, pay64);
        NBStructMap_addStructM(map, s, key, NTNCoreCfgSslKey_getSharedStructMap());
        TNCoreCfgSslCertSrc_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgSslCertSrc_sharedStructMap);
    return TNCoreCfgSslCertSrc_sharedStructMap.map;
}

//TNCoreCfgSslCert

STNBStructMapsRec TNCoreCfgSslCert_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgSslCert_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgSslCert_sharedStructMap);
    if(TNCoreCfgSslCert_sharedStructMap.map == NULL){
        STTNCoreCfgSslCert s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgSslCert);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addBoolM(map, s, isRequested);
        NBStructMap_addBoolM(map, s, isRequired);
        NBStructMap_addStructM(map, s, source, TNCoreCfgSslCertSrc_getSharedStructMap());
        TNCoreCfgSslCert_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgSslCert_sharedStructMap);
    return TNCoreCfgSslCert_sharedStructMap.map;
}

//TNCoreCfgSsl

STNBStructMapsRec TNCoreCfgSsl_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgSsl_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgSsl_sharedStructMap);
    if(TNCoreCfgSsl_sharedStructMap.map == NULL){
        STTNCoreCfgSsl s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgSsl);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addStructM(map, s, cert, TNCoreCfgSslCert_getSharedStructMap());
        NBStructMap_addPtrToArrayOfStructM(map, s, cas, casSz, ENNBStructMapSign_Unsigned, TNCoreCfgSslCertSrc_getSharedStructMap());
        TNCoreCfgSsl_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgSsl_sharedStructMap);
    return TNCoreCfgSsl_sharedStructMap.map;
}

//TNCoreCfgMask

STNBStructMapsRec TNCoreCfgMask_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgMask_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgMask_sharedStructMap);
    if(TNCoreCfgMask_sharedStructMap.map == NULL){
        STTNCoreCfgMask s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgMask);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addUIntM(map, s, seed);
        TNCoreCfgMask_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgMask_sharedStructMap);
    return TNCoreCfgMask_sharedStructMap.map;
}

//TNCoreCfgRedir

STNBStructMapsRec TNCoreCfgRedir_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgRedir_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgRedir_sharedStructMap);
    if(TNCoreCfgRedir_sharedStructMap.map == NULL){
        STTNCoreCfgRedir s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgRedir);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addStrPtrM(map, s, server);
        NBStructMap_addUIntM(map, s, port);
        NBStructMap_addPtrToArrayOfStrPtrM(map, s, layers, layersSz, ENNBStructMapSign_Unsigned);
        NBStructMap_addStructM(map, s, ssl, TNCoreCfgSsl_getSharedStructMap());
        NBStructMap_addStructM(map, s, mask, TNCoreCfgMask_getSharedStructMap());
        TNCoreCfgRedir_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgRedir_sharedStructMap);
    return TNCoreCfgRedir_sharedStructMap.map;
}

//TNCoreCfgPort

STNBStructMapsRec TNCoreCfgPort_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgPort_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfgPort_sharedStructMap);
    if(TNCoreCfgPort_sharedStructMap.map == NULL){
        STTNCoreCfgPort s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgPort);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addUIntM(map, s, port);
        NBStructMap_addPtrToArrayOfStrPtrM(map, s, layers, layersSz, ENNBStructMapSign_Unsigned);
        NBStructMap_addStructM(map, s, ssl, TNCoreCfgSsl_getSharedStructMap());
        NBStructMap_addStructM(map, s, mask, TNCoreCfgMask_getSharedStructMap());
        NBStructMap_addStructM(map, s, redir, TNCoreCfgRedir_getSharedStructMap());
        TNCoreCfgPort_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgPort_sharedStructMap);
    return TNCoreCfgPort_sharedStructMap.map;
}

//TNCoreCfgPorts

STNBStructMapsRec TNCoreCfgPorts_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgPorts_getSharedStructMap(void) {
    NBMngrStructMaps_lock(&TNCoreCfgPorts_sharedStructMap);
    if (TNCoreCfgPorts_sharedStructMap.map == NULL) {
        STTNCoreCfgPorts s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgPorts);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addPtrToArrayOfStructM(map, s, ports, portsSz, ENNBStructMapSign_Unsigned, TNCoreCfgPort_getSharedStructMap());
        TNCoreCfgPorts_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgPorts_sharedStructMap);
    return TNCoreCfgPorts_sharedStructMap.map;
}

//TNCoreCfgCAs

STNBStructMapsRec TNCoreCfgCAs_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfgCAs_getSharedStructMap(void) {
    NBMngrStructMaps_lock(&TNCoreCfgCAs_sharedStructMap);
    if (TNCoreCfgCAs_sharedStructMap.map == NULL) {
        STTNCoreCfgCAs s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfgCAs);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addPtrToArrayOfStructM(map, s, cas, casSz, ENNBStructMapSign_Unsigned, TNCoreCfgSslCertSrc_getSharedStructMap());
        TNCoreCfgCAs_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfgCAs_sharedStructMap);
    return TNCoreCfgCAs_sharedStructMap.map;
}

//TNCoreCfg

STNBStructMapsRec TNCoreCfg_sharedStructMap = STNBStructMapsRec_empty;

const STNBStructMap* TNCoreCfg_getSharedStructMap(void){
    NBMngrStructMaps_lock(&TNCoreCfg_sharedStructMap);
    if(TNCoreCfg_sharedStructMap.map == NULL){
        STTNCoreCfg s;
        STNBStructMap* map = NBMngrStructMaps_allocTypeM(STTNCoreCfg);
        NBStructMap_init(map, sizeof(s));
        NBStructMap_addPtrToArrayOfStructM(map, s, cas, casSz, ENNBStructMapSign_Unsigned, TNCoreCfgSslCertSrc_getSharedStructMap());
        NBStructMap_addPtrToArrayOfStructM(map, s, ports, portsSz, ENNBStructMapSign_Unsigned, TNCoreCfgPort_getSharedStructMap());
        NBStructMap_addStructPtrM(map, s, io, TNCoreCfgPort_getSharedStructMap());
        TNCoreCfg_sharedStructMap.map = map;
    }
    NBMngrStructMaps_unlock(&TNCoreCfg_sharedStructMap);
    return TNCoreCfg_sharedStructMap.map;
}
