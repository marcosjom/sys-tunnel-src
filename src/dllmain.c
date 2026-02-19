// dllmain.cpp : Defines the entry point for the DLL application.

#include "nb/NBFrameworkDefs.h"
#include "dllmain.h"

#include "nb/NBFrameworkPch.h"
//
#include "nb/core/NBMngrStructMaps.h"
#include "nb/core/NBMngrProcess.h"
#include "nb/core/NBStruct.h"
#include "nb/net/NBSocket.h"
#include "nb/files/NBFile.h"
#include "core/TNCore.h"
#include "core/TNLyrMask.h"
//
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <stdlib.h>     /* srand, rand */
#include <time.h>       /* time */

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        //init
        {
            NBMngrProcess_init();
            NBMngrStructMaps_init();
            NBSocket_initEngine();
            //rand initialization
            srand((UI32)time(NULL));
        }
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        //uninit
        {
            NBSocket_releaseEngine();
            NBMngrStructMaps_release();
            NBMngrProcess_release();
        }
        break;
    }
    return TRUE;
}

__declspec(dllexport) void* __cdecl TNCoreAlloc(void) {
    STTNCoreRef* core = NBMemory_allocType(STTNCoreRef);
    *core = TNCore_alloc(NULL);
    return core;
}

__declspec(dllexport) void __cdecl TNCoreRelease(void* pCore) {
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            TNCore_stopFlag(*core);
            while (TNCore_isRunning(*core)) {
                //just wait
            }
            TNCore_release(core);
            TNCore_null(core);
        }
        NBMemory_free(core);
        core = NULL;
    }
}

//

//Add one CA certificate to the global list of CAs.

__declspec(dllexport) bool __cdecl TNCoreAddCA(void* pCore, const char* jsonCfg) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            STTNCoreCfgSslCertSrc cfg;
            NBMemory_setZeroSt(cfg, STTNCoreCfgSslCertSrc);
            if (NBStruct_stReadFromJsonStr(jsonCfg, NBString_strLenBytes(jsonCfg), TNCoreCfgSslCertSrc_getSharedStructMap(), &cfg, sizeof(cfg))) {
#               ifdef NB_CONFIG_INCLUDE_ASSERTS
                {
                    STNBString str;
                    NBString_init(&str);
                    NBStruct_stConcatAsJson(&str, TNCoreCfgSslCertSrc_getSharedStructMap(), &cfg, sizeof(cfg));
                    PRINTF_INFO("TNCoreAddCA parsed:-->\n%s<--\n", str.str);
                    NBString_release(&str);
                }
#               endif
                r = TNCore_addCA(*core , &cfg);
            }
            NBStruct_stRelease(TNCoreCfgSslCertSrc_getSharedStructMap(), &cfg, sizeof(cfg));
        }
    }
    return  r;
}

//Add one or more CAs certificates to the global list of CAs.

__declspec(dllexport) bool __cdecl TNCoreAddCAs(void* pCore, const char* jsonCfg) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            STTNCoreCfgCAs cfg;
            NBMemory_setZeroSt(cfg, STTNCoreCfgCAs);
            if (NBStruct_stReadFromJsonStr(jsonCfg, NBString_strLenBytes(jsonCfg), TNCoreCfgCAs_getSharedStructMap(), &cfg, sizeof(cfg))) {
#               ifdef NB_CONFIG_INCLUDE_ASSERTS
                {
                    STNBString str;
                    NBString_init(&str);
                    NBStruct_stConcatAsJson(&str, TNCoreCfgCAs_getSharedStructMap(), &cfg, sizeof(cfg));
                    PRINTF_INFO("TNCoreAddCAs parsed:-->\n%s<--\n", str.str);
                    NBString_release(&str);
                }
#               endif
                r = TNCore_addCAs(*core, &cfg);
            }
            NBStruct_stRelease(TNCoreCfgCAs_getSharedStructMap(), &cfg, sizeof(cfg));
        }
    }
    return  r;
}

//Add one port to the core.

__declspec(dllexport) bool __cdecl TNCoreAddPort(void* pCore, const char* jsonCfg) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            STTNCoreCfgPort cfg;
            NBMemory_setZeroSt(cfg, STTNCoreCfgPort);
            if (NBStruct_stReadFromJsonStr(jsonCfg, NBString_strLenBytes(jsonCfg), TNCoreCfgPort_getSharedStructMap(), &cfg, sizeof(cfg))) {
#               ifdef NB_CONFIG_INCLUDE_ASSERTS
                {
                    STNBString str;
                    NBString_init(&str);
                    NBStruct_stConcatAsJson(&str, TNCoreCfgPort_getSharedStructMap(), &cfg, sizeof(cfg));
                    PRINTF_INFO("TNCoreAddPort parsed:-->\n%s<--\n", str.str);
                    NBString_release(&str);
                }
#               endif
                r = TNCore_addPort(*core, &cfg);
            }
            NBStruct_stRelease(TNCoreCfgPort_getSharedStructMap(), &cfg, sizeof(cfg));
        }
    }
    return  r;
}

//Add one or more ports to the core.

__declspec(dllexport) bool __cdecl TNCoreAddPorts(void* pCore, const char* jsonCfg) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            STTNCoreCfgPorts cfg;
            NBMemory_setZeroSt(cfg, STTNCoreCfgPorts);
            if (NBStruct_stReadFromJsonStr(jsonCfg, NBString_strLenBytes(jsonCfg), TNCoreCfgPorts_getSharedStructMap(), &cfg, sizeof(cfg))) {
#               ifdef NB_CONFIG_INCLUDE_ASSERTS
                {
                    STNBString str;
                    NBString_init(&str);
                    NBStruct_stConcatAsJson(&str, TNCoreCfgPorts_getSharedStructMap(), &cfg, sizeof(cfg));
                    PRINTF_INFO("TNCoreAddPorts parsed:-->\n%s<--\n", str.str);
                    NBString_release(&str);
                }
#               endif
                r = TNCore_addPorts(*core, &cfg);
            }
            NBStruct_stRelease(TNCoreCfgPorts_getSharedStructMap(), &cfg, sizeof(cfg));
        }
    }
    return  r;
}

//Prepare (finalize config)

__declspec(dllexport) bool __cdecl TNCorePrepare(void* pCore) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core) && TNCore_prepare(*core, NULL)) {
            r = true;
        }
    }
    return  r;
}

//Starts listening port.

__declspec(dllexport) bool __cdecl TNCoreStartListening(void* pCore) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core) && TNCore_startListening(*core)) {
            r = true;
        }
    }
    return  r;
}

//Runs at this thread (locks it)

__declspec(dllexport) bool __cdecl TNCoreRunAtThisThread(void* pCore) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core) && TNCore_runAtThisThread(*core)) {
            r = true;
        }
    }
    return  r;
}

//IsRunning

__declspec(dllexport) bool __cdecl TNCoreIsRunning(void* pCore) {
    bool r = false;
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core) && TNCore_isRunning(*core)) {
            r = true;
        }
    }
    return  r;
}

//Flags the core to stop and clean resources, and returns inmediately.

__declspec(dllexport) void __cdecl TNCoreStopFlag(void* pCore) {
    if (pCore != NULL) {
        STTNCoreRef* core = (STTNCoreRef*)pCore;
        if (TNCore_isSet(*core)) {
            TNCore_stopFlag(*core);
        }
    }
}

//Mask data

__declspec(dllexport) unsigned char __cdecl TNLryMaskEncode(unsigned char seed, void* buff, unsigned int buffSz){
    return (unsigned char)TNLryMask_encode((UI8)seed, buff, buffSz); //returns the new seed for next call
}

//Unmask data

__declspec(dllexport) unsigned char __cdecl TNLryMaskDecode(unsigned char seed, void* buff, unsigned int buffSz){
    return (unsigned char)TNLryMask_decode((UI8)seed, buff, buffSz); //returns the new seed for next call
}
