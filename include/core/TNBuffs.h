//
//  TNBuffs.h
//  sys-tunnel
//
//  Created by Marcos Ortega on 15/3/23.
//

#ifndef TNBuffs_h
#define TNBuffs_h

#include "nb/NBFrameworkDefs.h"
//

#ifdef __cplusplus
extern "C" {
#endif

    //About buffer sizes: 
    //
    //  2023/may/03, Windows 11
    //  Win11 iperf3 TCP test
    //      native (non-tunnel) = 11.5 Gbps
    //  Win11 iperf3 redir iperf-clt to tcp-tunnel to iperf-sever:
    //      16KB buffs = 6.1 Gbps
    //      32KB buffs = 10.7 Gbps
    //      64KB buffs = 14.0 Gbps
    //      128KB buffs = 15.4 Gbps
    //      256KB buffs =  15.6 Gbps
    //  MacBookPro2019 Ventura 13.1 iperf3 TCP test
    //      native (non-tunnel) = 57.7 Gbps
    //  Win11 iperf3 redir iperf-clt to tcp-tunnel to iperf-sever:
    //      16KB buffs = 12.8 Gbps
    //      32KB buffs = 20.6 Gbps
    //      64KB buffs = 26.9 Gbps
    //      128KB buffs = 31.2 Gbps
    //      256KB buffs = 32.4 Gbps
    //  Just as reference for buffers sizes,
    //  other improvements were made after these.
    //
    #define TN_CORE_CONN_BUFF_SZ               (1024 * 128) 

    //TNBuff (one buffer)

    typedef struct STTNBuff_ {
        BYTE*   data;
        UI32    size;    //right bo
        UI32    csmd;    //left border
        UI32    filled;  //right border
    } STTNBuff;

    //TNBuffs (chain of buffers)

    //Note: these buffers are designed for one-fill-action per buffer.
    //Their purpose is to provide 'big-enough' buffers for every low-level-call, like 'rcv' or 'read'.
    //If an empty buffer is available the next low-level-call will be made to that empty buffer,
    //else, the next low-level-call will be made to the remain space of the last buffer available.

    typedef struct STTNBuffs_ {
        STTNBuff* fill;  //current filling buffer (write)
        STTNBuff* read;  //current consume buffer (read)
        //allocs (two buffers, allows simultanious read/write by allowing a second buffer if the primary buffer is 'locked')
        struct {
            STTNBuff buff0;
            STTNBuff buff1;
        } allocs;
        //totals
        struct {
            UI64    csmd;      //total filled-bytes consumed from buffer
            UI64    filled;    //total filled-bytes populated on buffer
        } totals;
    } STTNBuffs;

    void TNBuffs_init(STTNBuffs* obj);
    void TNBuffs_release(STTNBuffs* obj);
    //
    BOOL TNBuffs_create(STTNBuffs* obj, const SI32 buffSize);
    //io
#   define  TNBuffs_canConsume(PTR)     ((PTR)->read != NULL && (PTR)->read->csmd < (PTR)->read->filled)
#   define  TNBuffs_canFill(PTR)        ((PTR)->fill != NULL && (PTR)->fill->filled < (PTR)->fill->size)
#   define  TNBuffs_canConsumeSz(PTR, SZ) ((PTR)->read != NULL && ((PTR)->read->csmd + (SZ)) <= (PTR)->read->filled)
#   define  TNBuffs_canFillSz(PTR, SZ)  ((PTR)->fill != NULL && ((PTR)->fill->filled + (SZ)) <= (PTR)->fill->size)
#   define  TNBuffs_csmAvailSz(PTR)     ((PTR)->read != NULL ? ((PTR)->read->filled - (PTR)->read->csmd) : 0ULL)
#   define  TNBuffs_fillAvailSz(PTR)    ((PTR)->fill != NULL ? ((PTR)->fill->size - (PTR)->fill->filled) : 0ULL)
    SI32 TNBuffs_consume(STTNBuffs* obj, void* dst, const SI32 dstSz);      //read buffer and move csm-cursor
    SI32 TNBuffs_fill(STTNBuffs* obj, const void* src, const SI32 srcSz);   //write buffer and move fill-cursor
    void TNBuffs_moveCsmCursor(STTNBuffs* obj, const SI32 moveSz);
    void TNBuffs_moveFillCursor(STTNBuffs* obj, const SI32 moveSz);

#ifdef __cplusplus
} //extern "C"
#endif

#endif /* TNBuffs_h */
