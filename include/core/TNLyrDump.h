//
//  TNLyrDump.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 05/03/26.
//

#ifndef TNLyrDump_h
#define TNLyrDump_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStopFlag.h"
//
#include "core/TNCoreCfg.h"
#include "core/TNLyrBase.h"

#ifdef __cplusplus
extern "C" {
#endif

    //TNLyrDump

    NB_OBJREF_HEADER(TNLyrDump)    //client connected

    BOOL TNLyrDump_getLyrItf(STTNLyrDumpRef ref, STTNLyrLstnr* dst);  //get this lyr iterface
    BOOL TNLyrDump_setParentStopFlag(STTNLyrDumpRef ref, STNBStopFlagRef* parentStopFlag);
    //
    BOOL TNLyrDump_prepare(STTNLyrDumpRef ref, const ENTNLyrFlow flow, const char* pathPrefix);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrDump_h */
