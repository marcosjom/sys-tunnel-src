//
//  TNLyrIO.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNLyrIO_h
#define TNLyrIO_h

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

    //TNLyrIO

    NB_OBJREF_HEADER(TNLyrIO)    //client connected

    BOOL TNLyrIO_getLyrItf(STTNLyrIORef ref, STTNLyrLstnr* dst);  //get this lyr iterface
    BOOL TNLyrIO_setPollsterSync(STTNLyrIORef ref, STNBIOPollsterSyncRef pollSync); //one external pollster only
    BOOL TNLyrIO_setPollstersProvider(STTNLyrIORef ref, STNBIOPollstersProviderRef provider); //multiple external pollsters
    BOOL TNLyrIO_setParentStopFlag(STTNLyrIORef ref, STNBStopFlagRef* parentStopFlag);

    //socket
    BOOL TNLyrIO_prepareOwningAcceptedSocket(STTNLyrIORef ref, STNBSocketRef socket);
    BOOL TNLyrIO_prepareConnecting(STTNLyrIORef ref, const char* server, const SI32 port);

    //file
    BOOL TNLyrIO_prepareAsStdIn(STTNLyrIORef ref);
    BOOL TNLyrIO_prepareAsStdOut(STTNLyrIORef ref);
    

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrIO_h */
