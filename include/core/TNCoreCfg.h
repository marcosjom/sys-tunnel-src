//
//  TNCoreCfg.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNCoreCfg_h
#define TNCoreCfg_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStructMap.h"

#ifdef __cplusplus
extern "C" {
#endif

//NTNCoreCfgSslKey

typedef struct STNTNCoreCfgSslKey_ {
    char*        pass;    //p12 password
    char*        path;    //p12 file (my-private-key + ca-certificate + my-certificate)
    char*        pay64;   //base-64 file payload
    char*        name;    //friendly name at p12
} STNTNCoreCfgSslKey;

const STNBStructMap* NTNCoreCfgSslKey_getSharedStructMap(void);

//TNCoreCfgSslCertSrc

typedef struct STTNCoreCfgSslCertSrc_ {
    char*              path;   //file-path
    char*              pay64;  //base-64 file payload
    STNTNCoreCfgSslKey key;
} STTNCoreCfgSslCertSrc;

const STNBStructMap* TNCoreCfgSslCertSrc_getSharedStructMap(void);

//TNCoreCfgSslCert

typedef struct STTNCoreCfgSslCert_ {
    BOOL                    isRequested;
    BOOL                    isRequired;
    STTNCoreCfgSslCertSrc   source;
} STTNCoreCfgSslCert;

const STNBStructMap* TNCoreCfgSslCert_getSharedStructMap(void);

//TNCoreCfgSsl

typedef struct STTNCoreCfgSsl_ {
    STTNCoreCfgSslCert      cert;
    STTNCoreCfgSslCertSrc*  cas;    //default certificate autorities
    UI32                    casSz;  //default certificate autorities
} STTNCoreCfgSsl;

const STNBStructMap* TNCoreCfgSsl_getSharedStructMap(void);

//TNCoreCfgMask

typedef struct STTNCoreCfgMask_ {
    UI8                     seed;
} STTNCoreCfgMask;

const STNBStructMap* TNCoreCfgMask_getSharedStructMap(void);

//TNCoreCfgRedir

typedef struct STTNCoreCfgRedir_ {
    char*               server;
    UI32                port;
    char**              layers;     //
    UI32                layersSz;   //
    STTNCoreCfgSsl      ssl;        //used only if there is a 'ssl' value in 'layers'.
    STTNCoreCfgMask     mask;       //used only if there is a 'mask' value in 'layers'.
} STTNCoreCfgRedir;

const STNBStructMap* TNCoreCfgRedir_getSharedStructMap(void);

//TNCoreCfgPort

typedef struct STTNCoreCfgPort_ {
    UI32                    port;
    char**                  layers;     //
    UI32                    layersSz;   //
    STTNCoreCfgSsl          ssl;        //used only if there is a 'ssl' value in 'layers'.
    STTNCoreCfgMask         mask;       //used only if there is a 'mask' value in 'layers'.
    STTNCoreCfgRedir        redir;
} STTNCoreCfgPort;

const STNBStructMap* TNCoreCfgPort_getSharedStructMap(void);

//TNCoreCfgPorts

typedef struct STTNCoreCfgPorts_ {
    STTNCoreCfgPort*        ports;
    UI32                    portsSz;
} STTNCoreCfgPorts;

const STNBStructMap* TNCoreCfgPorts_getSharedStructMap(void);

//TNCoreCfgCAs

typedef struct STTNCoreCfgCAs_ {
    STTNCoreCfgSslCertSrc*  cas;    //default certificate autorities
    UI32                    casSz;  //default certificate autorities
} STTNCoreCfgCAs;

const STNBStructMap* TNCoreCfgCAs_getSharedStructMap(void);

//TNCoreCfg

typedef struct STTNCoreCfg_ {
    STTNCoreCfgSslCertSrc*  cas;    //default certificate autorities
    UI32                    casSz;  //default certificate autorities
    STTNCoreCfgPort*        ports;  //listening ports
    UI32                    portsSz;//listening ports
    STTNCoreCfgPort*        io;  //listening stdin (optional)
} STTNCoreCfg;

const STNBStructMap* TNCoreCfg_getSharedStructMap(void);


#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNCoreCfg_h */
