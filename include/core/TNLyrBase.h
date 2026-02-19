//
//  TNLyrBase.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNLyrBase_h
#define TNLyrBase_h

#include "nb/NBFrameworkDefs.h"
//

#ifdef __cplusplus
extern "C" {
#endif

    //ENTNLyrFlow

    typedef enum ENTNLyrFlow_ {
        ENTNLyrFlow_Undefined = 0,  //implicit, undefined
        ENTNLyrFlow_FromLwr,        //when raw-data comes from lowerLyr
        ENTNLyrFlow_FromUp,         //when raw-data comes from upLyr
        //
        ENTNLyrFlow_Count
    } ENTNLyrFlow;

    //STTNLyrLstnr

    struct STTNLyrLstnr_;

    //STTNLyrLstnrItf (data pushed to next lyr)

    typedef struct STTNLyrLstnrItf_ {
        //retain/release
        void (*lyrRetain)(void* usrData);
        void (*lyrRelease)(void* usrData);
        //lyr stack
        BOOL (*lyrSetNext)(const struct STTNLyrLstnr_* nxt, void* usrData); //set next lyr iterface
        //data flow
        void (*lyrStart)(void* usrData);        //start processing and start uppperLyr
        BOOL (*lyrIsRunning)(void* usrData);    //cleanup must wait
        void (*lyrConsumeMask)(const STNBIOLnk* lnk, const UI8 pollMask, UI8* dstPollReq, void* usrData);
        void (*lyrShutdown)(const UI8 mask, void* usrData); //NB_IO_BIT_READ | NB_IO_BIT_WRITE
        void (*lyrClose)(void* usrData);
        //dbg
        void (*lyrConcat)(STNBString* dst, void* usrData);
    } STTNLyrLstnrItf;

    //STTNLyrLstnr (data pushed to next lyr)

    typedef struct STTNLyrLstnr_ {
        STTNLyrLstnrItf    itf;
        void*              usrParam;
    } STTNLyrLstnr;

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNLyrBase_h */
