//
//  TNLyrMask.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNLyrMask_h
#define TNLyrMask_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStopFlag.h"
//
#include "core/TNCoreCfg.h"
#include "core/TNLyrBase.h"

#ifdef __cplusplus
extern "C" {
#endif

    //TNLyrMask

    NB_OBJREF_HEADER(TNLyrMask)    //client connected

    BOOL TNLyrMask_getLyrItf(STTNLyrMaskRef ref, STTNLyrLstnr* dst);  //get this lyr iterface
    BOOL TNLyrMask_setParentStopFlag(STTNLyrMaskRef ref, STNBStopFlagRef* parentStopFlag);
    //
    BOOL TNLyrMask_prepare(STTNLyrMaskRef ref, const ENTNLyrFlow flow, const UI8 seed);
    //
    UI8  TNLryMask_encode(UI8 seed, void* data, const UI32 dataSz);      //returns the new seed for next call
    UI8  TNLryMask_decode(UI8 seed, void* data, const UI32 dataSz);    //returns the new seed for next call

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrMask_h */
