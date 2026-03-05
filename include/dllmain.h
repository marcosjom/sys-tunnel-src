#ifndef dellmain_h
#define dellmain_h

#ifdef __cplusplus
extern "C" {
#endif

	//
	__declspec(dllexport) void* __cdecl TNCoreAlloc(void);
	__declspec(dllexport) void __cdecl TNCoreRelease(void* core);
	//
	
	//Add one CA certificate to the global list of CAs.
	//STTNCoreCfgSslCertSrc
	/*
	* {
	*    "path": string
	*    , "pay64": string
	* }
	*/
	__declspec(dllexport) bool __cdecl TNCoreAddCA(void* core, const char* jsonCfg);

	//Add one or more CAs certificates to the global list of CAs.
	//STTNCoreCfgCAs
	/*
	* {
	*    "cas": [ STTNCoreCfgSslCertSrc, ... ] (view structure at 'TNCoreAddCA' method)
	* }
	*/
	__declspec(dllexport) bool __cdecl TNCoreAddCAs(void* core, const char* jsonCfg);

	//Add one port to the core.
	//STTNCoreCfgPort
	/*
	* {
	*    "port": number
	*    , "layers": [string, ...] ("ssl", "mask", "base64", "dump", ...)
	*    , "ssl": {
	*       "cert": {
	*          "isRequested": bool
	*          , "isRequired": bool
	*          , "source": {
	*             "path": string
	*             , "pay64": string
	*             , "key": {
	*                "pass": string
	*                , "path": string
	*                , "pay64": string
	*                , "name": string
	*             }
	*          }
	*       }
	*    }
	*    , "mask": {
	*       "seed": number-0-255
	*    }
    *    , "dump": {
    *       "pathPrefix": "./folder/namePrefix_"
    *    }
	*    , "redir": {
	*       , "server": string
	*       , "port": number
	*       , "layers": [string, ...] ("ssl", "mask", "base64", "dump", ...)
	*       , "ssl": {
	*          "cert": {
	*             "isRequested": bool
	*             , "isRequired": bool
	*             , "source": {
	*                "path": string
	*                , "pay64": string
	*                , "key": {
	*                   "pass": string
	*                   , "path": string
	*                   , "pay64": string
	*                   , "name": string
	*                }
	*             }
	*          }
	*       }
	*       , "mask": {
	*          "seed": number-0-255
	*       }
    *       , "dump": {
    *          "pathPrefix": "./folder/namePrefix_"
    *       }
	*    }
	* }
	*/
	__declspec(dllexport) bool __cdecl TNCoreAddPort(void* core, const char* jsonCfg);

	//Add one or more ports to the core.
	//STTNCoreCfgPorts
	/*
	* {
	*   "ports": [ STTNCoreCfgPort, ...] (view structure at 'TNCorePreparePort' method)
	* }
	*/
	__declspec(dllexport) bool __cdecl TNCoreAddPorts(void* core, const char* jsonCfg);


	//Prepare (finalize config)

	__declspec(dllexport) bool __cdecl TNCorePrepare(void* core);

	//Starts listening port.
	
	__declspec(dllexport) bool __cdecl TNCoreStartListening(void* core);

	//Runs at this thread (locks it)

	__declspec(dllexport) bool __cdecl TNCoreRunAtThisThread(void* core);

	//Is running

	__declspec(dllexport) bool __cdecl TNCoreIsRunning(void* core);

	//Flags the core to stop and clean resources, and returns inmediately.

	__declspec(dllexport) void __cdecl TNCoreStopFlag(void* core);

    //Mask data
    __declspec(dllexport) unsigned char __cdecl TNLryMaskEncode(unsigned char seed, void* buff, unsigned int buffSz);

    //Unmask data
    __declspec(dllexport) unsigned char __cdecl TNLryMaskDecode(unsigned char seed, void* buff, unsigned int buffSz);


#ifdef __cplusplus
}
#endif

#endif
