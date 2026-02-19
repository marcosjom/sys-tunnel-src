//
//  TNCore.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNCore_h
#define TNCore_h

#include "nb/NBFrameworkDefs.h"
#include "nb/core/NBStructMap.h"
#include "nb/core/NBStopFlag.h"
#include "nb/core/NBIOPollster.h"
#include "nb/core/NBIOPollstersProvider.h"
//
#include "core/TNCoreCfg.h"

#ifdef __cplusplus
extern "C" {
#endif

//TNCoreArgs

typedef struct STTNCoreArgs_ {
    STTNCoreCfg cfg;
    UI32        secsRunAndExit;     //secs to run and auto-stop
    UI32        maxSecsWithoutConn; //max secs to allow without first/next client conn (auto-stops)
    UI32        maxConnsAndExit;    //max conns to receive (stops receiving after) and exits after last conn leaves.
    BOOL        printArgs;          //prints parsed/loaded args
    BOOL        printCfg;           //prints parsed/loaded cfg-args
    BOOL        printHelp;          //prints help
} STTNCoreArgs;

const STNBStructMap* TNCoreArgs_getSharedStructMap(void);

NB_OBJREF_HEADER(TNCore)

//
BOOL TNCore_parseArgs(const int argc, const char* argv[], STTNCoreArgs* dst, STNBString* dstErrLst);
BOOL TNCore_concatArsgHelp(const char* exeFilename, STNBString* dst);
//
BOOL TNCore_setPollster(STTNCoreRef ref, STNBIOPollsterSyncRef pollSync); //one external pollster only
BOOL TNCore_setPollstersProvider(STTNCoreRef ref, STNBIOPollstersProviderRef provider); //multiple external pollsters
BOOL TNCore_setParentStopFlag(STTNCoreRef ref, STNBStopFlagRef* parentStopFlag);
//one-step-prepare
BOOL TNCore_prepare(STTNCoreRef ref, const STTNCoreArgs* args);
//multiple-steps-prepare
BOOL TNCore_addCA(STTNCoreRef ref, const STTNCoreCfgSslCertSrc* cfg);
BOOL TNCore_addCAs(STTNCoreRef ref, const STTNCoreCfgCAs* cfg);
BOOL TNCore_addPort(STTNCoreRef ref, const STTNCoreCfgPort* cfg);
BOOL TNCore_addPorts(STTNCoreRef ref, const STTNCoreCfgPorts* cfg);
//
BOOL TNCore_startListening(STTNCoreRef ref);
BOOL TNCore_runAtThisThread(STTNCoreRef ref);
BOOL TNCore_isRunning(STTNCoreRef ref);
void TNCore_stopFlag(STTNCoreRef ref);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNCore_h */
