//
//  TNLyrBase64.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNLyrBase64_h
#define TNLyrBase64_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStopFlag.h"
//
#include "core/TNCoreCfg.h"
#include "core/TNLyrBase.h"

#ifdef __cplusplus
extern "C" {
#endif

    //TNLyrBase64

    NB_OBJREF_HEADER(TNLyrBase64)    //client connected

    BOOL TNLyrBase64_getLyrItf(STTNLyrBase64Ref ref, STTNLyrLstnr* dst);  //get this lyr iterface
    BOOL TNLyrBase64_setParentStopFlag(STTNLyrBase64Ref ref, STNBStopFlagRef* parentStopFlag);
    //
    BOOL TNLyrBase64_prepare(STTNLyrBase64Ref ref, const ENTNLyrFlow flow);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrBase64_h */
