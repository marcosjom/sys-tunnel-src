//
//  TNCorePort.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNCorePort_h
#define TNCorePort_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStopFlag.h"
#include "nb/core/NBIOPollster.h"
#include "nb/core/NBIOPollstersProvider.h"
#include "nb/ssl/NBSslContext.h"
//
#include "core/TNCoreCfg.h"

#ifdef __cplusplus
extern "C" {
#endif

NB_OBJREF_HEADER(TNCorePort)

typedef struct STTNCorePortLstnrItf_ {
    BOOL (*portConnArrived)(STTNCorePortRef ref, STNBSocketRef socket, const STTNCoreCfgPort* cfg, STNBSslContextRef sslCtx, const STNBX509* sslCAs, const UI32 sslCAsSz, STNBSslContextRef redirSslCtx, const STNBX509* redirSslCAs, const UI32 redirSslCAsSz, void* usrParam);
} STTNCorePortLstnrItf;

//
BOOL TNCorePort_setPollster(STTNCorePortRef ref, STNBIOPollsterSyncRef pollSync); //one external pollster only
BOOL TNCorePort_setPollstersProvider(STTNCorePortRef ref, STNBIOPollstersProviderRef provider); //multiple external pollsters
BOOL TNCorePort_setParentStopFlag(STTNCorePortRef ref, STNBStopFlagRef* parentStopFlag);
//
BOOL TNCorePort_setListener(STTNCorePortRef ref, STTNCorePortLstnrItf* itf, void* usrParam);
//
UI32 TNCorePort_getPort(STTNCorePortRef ref);
BOOL TNCorePort_prepare(STTNCorePortRef ref, const STTNCoreCfgPort* cfg, STNBX509* globalCAs, const UI32 globalCAsSz);
BOOL TNCorePort_startListening(STTNCorePortRef ref);
BOOL TNCorePort_isBusy(STTNCorePortRef ref);
void TNCorePort_stopFlag(STTNCorePortRef ref);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNCorePort_h */
