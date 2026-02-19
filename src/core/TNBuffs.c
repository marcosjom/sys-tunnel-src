//
//  TNBuffs.c
//  sys-tunnel-core-osx
//
//  Created by Marcos Ortega on 15/3/23.
//

#include "nb/NBFrameworkPch.h"
#include "nb/NBObject.h"
#include "nb/core/NBMemory.h"
#include "nb/core/NBIO.h"
#include "core/TNBuffs.h"

//TNBuffs

void TNBuffs_init(STTNBuffs* obj){
    NBMemory_setZeroSt(*obj, STTNBuffs);
}

void TNBuffs_release(STTNBuffs* obj){
    obj->fill = obj->read = NULL;
    //allocs
    {
        //buff0
        {
            STTNBuff* buff = &obj->allocs.buff0;   //final-buff if no-ssl, encrypted-buff if ssl
            if(buff->data != NULL){
                NBMemory_free(buff->data);
                buff->data = NULL;
            }
            buff->filled = buff->csmd = buff->size = 0;
        }
        //buff1
        {
            STTNBuff* buff = &obj->allocs.buff1;   //final-buff if no-ssl, encrypted-buff if ssl
            if(buff->data != NULL){
                NBMemory_free(buff->data);
                buff->data = NULL;
            }
            buff->filled = buff->csmd = buff->size = 0;
        }
    }
}

BOOL TNBuffs_create(STTNBuffs* obj, const SI32 buffSize){
    BOOL r = FALSE;
    if(obj->read == NULL && obj->fill == NULL){
        //buff0
        {
            STTNBuff* buff = &obj->allocs.buff0;
            NBASSERT(buff->data == NULL && buff->size == 0)
            if(buff->data != NULL){
                NBMemory_free(buff->data);
                buff->data = NULL;
                buff->size = 0;
            }
            buff->size = buffSize;
#           ifdef NB_CONFIG_INCLUDE_ASSERTS
            buff->data = (BYTE*)NBMemory_alloc(buff->size + 1); //+1 for a '\0'
#           else
            buff->data = (BYTE*)NBMemory_alloc(buff->size);
#           endif
        }
        //buff1
        {
            STTNBuff* buff = &obj->allocs.buff1;
            NBASSERT(buff->data == NULL && buff->size == 0)
            if(buff->data != NULL){
                NBMemory_free(buff->data);
                buff->data = NULL;
                buff->size = 0;
            }
            buff->size = buffSize;
#           ifdef NB_CONFIG_INCLUDE_ASSERTS
            buff->data = (BYTE*)NBMemory_alloc(buff->size + 1); //+1 for a '\0'
#           else
            buff->data = (BYTE*)NBMemory_alloc(buff->size);
#           endif
        }
        //activate firtst buffer
        obj->fill = obj->read = &obj->allocs.buff0;
        r = TRUE;
    }
    return r;
}

SI32 TNBuffs_consume(STTNBuffs* obj, void* pDst, const SI32 dstSz){  //read buffer and move csm-cursor
    SI32 r = NB_IO_ERROR;
    if(pDst != NULL && dstSz >= 0 && obj->read != NULL){
        BYTE* dst = (BYTE*)pDst;
        r = 0;
        while(r < dstSz && obj->read->csmd < obj->read->filled){
            const SI32 dstAvail     = (dstSz - r);
            const SI32 csmAvail     = (obj->read->filled - obj->read->csmd);
            const SI32 moveSz       = (dstAvail < csmAvail ? dstAvail : csmAvail);
            NBASSERT(moveSz > 0)
            NBMemory_copy(&dst[r], &obj->read->data[obj->read->csmd], moveSz);
            r += moveSz;
            //move cursor
            TNBuffs_moveCsmCursor(obj, moveSz);
        }
    }
    return r;
}

SI32 TNBuffs_fill(STTNBuffs* obj, const void* pSrc, const SI32 srcSz){     //write buffer and move fill-cursor
    SI32 r = NB_IO_ERROR;
    if(pSrc != NULL && srcSz >= 0 && obj->fill != NULL){
        const BYTE* src = (const BYTE*)pSrc;
        r = 0;
        while(r < srcSz && obj->fill->filled < obj->fill->size){
            const SI32 srcAvail     = (srcSz - r);
            const SI32 fillAvail    = (obj->fill->size - obj->fill->filled);
            const SI32 moveSz       = (srcAvail < fillAvail ? srcAvail : fillAvail);
            NBASSERT(moveSz > 0)
            NBMemory_copy(&obj->fill->data[obj->fill->filled], &src[r], moveSz);
            r += moveSz;
            //move cursor
            TNBuffs_moveFillCursor(obj, moveSz);
        }
    }
    return r;
}

//Note: these buffers are designed for one-fill-action per buffer.
//Their purpose is to provide 'big-enough' buffers for every low-level-call, like 'rcv' or 'read'.
//If an empty buffer is available the next low-level-call will be made to that empty buffer,
//else, the next low-level-call will be made to the remain space of the last buffer available.

void TNBuffs_moveFillCursor(STTNBuffs* obj, const SI32 moveSz){
    if(moveSz > 0){
        obj->fill->filled   += moveSz;
        obj->totals.filled  += moveSz;
        //swap fill-buffer
        NBASSERT(obj->fill->filled <= obj->fill->size)
        if(
           obj->fill->filled == obj->fill->size //buffer is full
           || obj->fill == obj->read //next buffer is not in use
           )
        {
            STTNBuff* nxt = (obj->fill == &obj->allocs.buff0 ? &obj->allocs.buff1 : &obj->allocs.buff0);
            if(nxt != obj->read){
                nxt->csmd = nxt->filled = 0;
                //also move 'read' buffer if consumed
                if(obj->read->csmd == obj->read->filled){
                    STTNBuff* nxt2 = (obj->read == &obj->allocs.buff0 ? &obj->allocs.buff1 : &obj->allocs.buff0);
                    obj->read = nxt2; NBASSERT(obj->read->csmd == 0)
                }
                obj->fill = nxt; NBASSERT(obj->fill->csmd == 0 && obj->fill->filled == 0)
            }
        }
    }
}

//Note: these buffers are designed for one-fill-action per buffer.
//Their purpose is to provide 'big-enough' buffers for every low-level-call, like 'rcv' or 'read'.
//If an empty buffer is available the next low-level-call will be made to that empty buffer,
//else, the next low-level-call will be made to the remain space of the last buffer available.

void TNBuffs_moveCsmCursor(STTNBuffs* obj, const SI32 moveSz){
    if(moveSz > 0){
        obj->read->csmd     += moveSz;
        obj->totals.csmd    += moveSz;
        //reset buffer
        NBASSERT(obj->read->csmd <= obj->read->filled)
        if(obj->read->csmd == obj->read->filled){
            if(obj->fill == obj->read){
                //reset buffer state
                obj->read->csmd = obj->fill->filled = 0;
            } else {
                //move to next buffer
                STTNBuff* nxt = (obj->read == &obj->allocs.buff0 ? &obj->allocs.buff1 : &obj->allocs.buff0);
                obj->read = nxt; NBASSERT(obj->read->csmd == 0)
            }
        }
    }
}
