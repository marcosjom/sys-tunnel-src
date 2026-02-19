//
//  TNLyrSsl.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNLyrSsl_h
#define TNLyrSsl_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStopFlag.h"
#include "nb/core/NBIOPollster.h"
#include "nb/core/NBIOPollstersProvider.h"
#include "nb/crypto/NBX509.h"
#include "nb/ssl/NBSsl.h"
#include "nb/ssl/NBSslContext.h"
//
#include "core/TNCoreCfg.h"
#include "core/TNLyrBase.h"

#ifdef __cplusplus
extern "C" {
#endif

    //TNLyrSsl

    NB_OBJREF_HEADER(TNLyrSsl)    //client connected

    BOOL TNLyrSsl_getLyrItf(STTNLyrSslRef ref, STTNLyrLstnr* dst);  //get this lyr iterface
    BOOL TNLyrSsl_setParentStopFlag(STTNLyrSslRef ref, STNBStopFlagRef* parentStopFlag);

    BOOL TNLyrSsl_startAcceptingConn(STTNLyrSslRef ref, STNBSslContextRef ctx, STNBX509* CAs, const UI32 CAsSz);
    BOOL TNLyrSsl_startConnect(STTNLyrSslRef ref, STNBSslContextRef ctx, STNBX509* CAs, const UI32 CAsSz);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrSsl_h */
