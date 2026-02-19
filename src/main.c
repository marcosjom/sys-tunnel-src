//
//  main.c
//  sys-tunnel-osx
//
//  Created by Marcos Ortega on 15/3/23.
//

#include "nb/NBFrameworkPch.h"
//
#include "nb/core/NBMngrProcess.h"
#include "nb/core/NBMngrStructMaps.h"
#include "nb/core/NBStruct.h"
#include "nb/core/NBStopFlag.h"
#include "nb/net/NBSocket.h"
//
#include "core/TNCore.h"
//
#include <stdlib.h>    //for rand()
#include <signal.h>    //for signal()
#include <time.h>      //for time()

static STNBStopFlagRef  _stopFlag;
static STTNCoreRef      _core;

#if !defined(_WIN32) && !defined(_WIN64)
typedef enum ENTNSignalAction_ {
    ENTNSignalAction_Ignore = 0,
    ENTNSignalAction_GracefullExit,
    ENTNSignalAction_Count
} ENTNSignalAction;
#endif

#if !defined(_WIN32) && !defined(_WIN64)
typedef struct STTNSignalDef_ {
    int              sig;
    const char*      sigName;
    ENTNSignalAction action;
} STTNSignalDef;
#endif

#if !defined(_WIN32) && !defined(_WIN64)
static STTNSignalDef _signalsDefs[] = {
    //sockets SIGPIPE signals (for unix-like systems)
    { SIGPIPE, "SIGPIPE", ENTNSignalAction_Ignore}, //Ignore
    //termination signals: https://www.gnu.org/software/libc/manual/html_node/Termination-Signals.html
    { SIGTERM, "SIGTERM", ENTNSignalAction_GracefullExit },
    { SIGINT, "SIGINT", ENTNSignalAction_GracefullExit },
    { SIGQUIT, "SIGQUIT", ENTNSignalAction_GracefullExit },
    { SIGKILL, "SIGKILL", ENTNSignalAction_GracefullExit },
    { SIGHUP, "SIGHUP", ENTNSignalAction_GracefullExit },
};
#endif

#if !defined(_WIN32) && !defined(_WIN64)
void TNMain_intHandler(int sig){
    SI32 i; const SI32 count = (sizeof(_signalsDefs) / sizeof(_signalsDefs[0]));
    for(i = 0; i < count; i++){
        const STTNSignalDef* def = &_signalsDefs[i];
        if(sig == def->sig){
            if(def->action == ENTNSignalAction_GracefullExit){
                PRINTF_INFO("MAIN, -------------------------------------.\n");
                PRINTF_INFO("MAIN, Server, captured signal %s...\n", def->sigName);
                if(NBStopFlag_isSet(_stopFlag)){
                    NBStopFlag_activate(_stopFlag);
                }
                PRINTF_INFO("MAIN, Server, clean-exit flag set (please wait ...).\n");
                PRINTF_INFO("MAIN, -------------------------------------.\n");
            }
            break;
        }
    }
}
#endif

void printHelp(const char* filepathExe, const char* errLst);

int main(int argc, const char * argv[]) {
    int r = 0;
    //Start engine
    NBMngrProcess_init();
    NBMngrStructMaps_init();
    NBSocket_initEngine();
    NBSocket_initWSA();
    srand((int)time(NULL));
    //Apply signal handlers.
    //Ignore SIGPIPE at process level (for unix-like systems)
#   if !defined(_WIN32) && !defined(_WIN64)
    {
        SI32 i; const SI32 count = (sizeof(_signalsDefs) / sizeof(_signalsDefs[0]));
        for(i = 0; i < count; i++){
            const STTNSignalDef* def = &_signalsDefs[i];
            if(def->action == ENTNSignalAction_Ignore){
                struct sigaction act;
                act.sa_handler    = SIG_IGN;
                act.sa_flags    = 0;
                sigemptyset(&act.sa_mask);
                sigaction(def->sig, &act, NULL);
            } else if(def->action == ENTNSignalAction_GracefullExit){
                struct sigaction act;
                act.sa_handler    = TNMain_intHandler;
                act.sa_flags    = 0;
                sigemptyset(&act.sa_mask);
                sigaction(def->sig, &act, NULL);
            }
        }
    }
#   endif
    //main
    {
        _stopFlag = NBStopFlag_alloc(NULL);
        _core = TNCore_alloc(NULL);
        //parse params
        {
            STTNCoreArgs args; STNBString errLst;
            NBMemory_setZeroSt(args, STTNCoreArgs);
            NBString_init(&errLst);
            if(!TNCore_parseArgs(argc, argv, &args, &errLst)){
                //print help
                printHelp(argv[0], errLst.str);
                //
                r = -1;
            } else {
                //print args
                if(args.printArgs){
                    STNBString cfgStr;
                    NBString_init(&cfgStr);
                    {
                        STNBStructConcatFormat fmt;
                        NBMemory_setZeroSt(fmt, STNBStructConcatFormat);
                        fmt.tabChar = " "; fmt.tabCharLen = 1;
                        fmt.ignoreZeroValues = TRUE;
                        fmt.objectsInNewLine = TRUE;
                        NBStruct_stConcatAsJsonWithFormat(&cfgStr, TNCoreArgs_getSharedStructMap(), &args, sizeof(args), &fmt);
                        if(errLst.length > 0){
                            PRINTF_CONSOLE("Errors parsing params:\n%s\n", errLst.str);
                        }
                        PRINTF_CONSOLE("Args loaded:\n%s\n", cfgStr.str);
                    }
                    NBString_release(&cfgStr);
                }
                //print cfg
                if(args.printCfg){
                    STNBString cfgStr;
                    NBString_init(&cfgStr);
                    {
                        STNBStructConcatFormat fmt;
                        NBMemory_setZeroSt(fmt, STNBStructConcatFormat);
                        fmt.tabChar = " "; fmt.tabCharLen = 1;
                        fmt.ignoreZeroValues = TRUE;
                        fmt.objectsInNewLine = TRUE;
                        NBStruct_stConcatAsJsonWithFormat(&cfgStr, TNCoreCfg_getSharedStructMap(), &args.cfg, sizeof(args.cfg), &fmt);
                        if(errLst.length > 0){
                            PRINTF_CONSOLE("Errors parsing params:\n%s\n", errLst.str);
                        }
                        PRINTF_CONSOLE("Config loaded:\n%s\n", cfgStr.str);
                    }
                    NBString_release(&cfgStr);
                }
                //print help
                if(args.printHelp){
                    printHelp(argv[0], NULL);
                }
                if(!TNCore_setParentStopFlag(_core, &_stopFlag)){
                    PRINTF_ERROR("MAIN, TNCore_setParentStopFlag failed.\n");
                    r = -1;
                } else if(!TNCore_prepare(_core, &args)){
                    PRINTF_ERROR("MAIN, TNCore_prepare failed.\n");
                    r = -1;
                } else if(!TNCore_startListening(_core)){
                    PRINTF_ERROR("MAIN, TNCore_startListening failed.\n");
                    r = -1;
                } else if(!TNCore_runAtThisThread(_core)){
                    PRINTF_ERROR("MAIN, TNCore_runAtThisThread failed.\n");
                    r = -1;
                } else {
                    PRINTF_INFO("MAIN, TNCore_runAtThisThread completed.\n");
                }
            }
            NBStruct_stRelease(TNCoreArgs_getSharedStructMap(), &args, sizeof(args));
            NBString_release(&errLst);
        }
        //stop
        if(NBStopFlag_isSet(_stopFlag)){
            NBStopFlag_activate(_stopFlag);
        }
        //release
        if(TNCore_isSet(_core)){
            TNCore_release(&_core);
            TNCore_null(&_core);
        }
        if(NBStopFlag_isSet(_stopFlag)){
            NBStopFlag_release(&_stopFlag);
            NBStopFlag_null(&_stopFlag);
        }
    }
    //End engine
    NBSocket_finishWSA();
    NBSocket_releaseEngine();
    NBMngrStructMaps_release();
    NBMngrProcess_release();
    return r;
}

void printHelp(const char* filepathExe, const char* errLst){
    STNBString help;
    NBString_init(&help);
    {
        TNCore_concatArsgHelp(filepathExe, &help);
        if(!NBString_strIsEmpty(errLst)){
            PRINTF_CONSOLE("Errors parsing params:\n%s\n", errLst);
        }
        PRINTF_CONSOLE("Command line help:\n%s\n", help.str);
    }
    NBString_release(&help);
}
