/*************************************************************************/ /*!
@File
@Title          Transport Layer kernel side API implementation.
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Transport Layer functions available to driver components in 
                the driver.
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

//#define PVR_DPF_FUNCTION_TRACE_ON 1
#undef PVR_DPF_FUNCTION_TRACE_ON
#include "pvr_debug.h"

#include "allocmem.h"
#include "pvrsrv_error.h"
#include "osfunc.h"

#include "pvr_tlcommon.h"
#include "tlintern.h"




/*
 * Make functions
 */


PTL_STREAM_DESC
TLMakeStreamDesc(PTL_SNODE f1, IMG_UINT32 f2, IMG_HANDLE f3)
{
	PTL_STREAM_DESC ps = OSAllocZMem(sizeof(TL_STREAM_DESC));
	if (ps == NULL) return NULL;
	ps->psNode = f1;
	ps->ui32Flags = f2;
	ps->hDataEvent = f3;
	return ps;

}

PTL_SNODE
TLMakeSNode(IMG_HANDLE f2, TL_STREAM *f3, TL_STREAM_DESC *f4)
{
	PTL_SNODE ps = OSAllocZMem(sizeof(TL_SNODE));
	if (ps == NULL) return NULL;
	ps->hDataEventObj = f2;
	ps->psStream = f3;
	ps->psDesc = f4;
	f3->psNode = ps;
	return ps;
}



/*
 * Transport Layer Global top variables and functions
 */
TL_GLOBAL_GDATA *TLGGD(void)	// TLGetGlobalData()
{
	static TL_GLOBAL_GDATA sTlGlobalData;

	return &sTlGlobalData;
}

IMG_VOID
TLSetGlobalRgxDevice(PVRSRV_DEVICE_NODE *psDevNode)
{
	PVR_ASSERT(psDevNode);
	TLGGD()->psRgxDevNode = (IMG_VOID*)psDevNode;
}

PVRSRV_DEVICE_NODE*
TLGetGlobalRgxDevice(void)
{
	PVRSRV_DEVICE_NODE* p = (PVRSRV_DEVICE_NODE*)(TLGGD()->psRgxDevNode);
	if (!p)
	{
		PVR_DPF((PVR_DBG_ERROR, "TLGetGlobalRgxDevice() NULL node ptr, TL " \
				"can not be used when no RGX device has been found"));
		PVR_ASSERT(p);
	}
	return p;
}

IMG_VOID TLAddStreamNode(PTL_SNODE psAdd)
{
	PVR_DPF_ENTERED;

	PVR_ASSERT(psAdd);
	psAdd->psNext = TLGGD()->psHead;
	TLGGD()->psHead = psAdd;
	
	PVR_DPF_RETURN;
}

PTL_SNODE TLFindStreamNodeByName(IMG_PCHAR pszName)
{
	TL_GLOBAL_GDATA* psGD = TLGGD();
	PTL_SNODE 		 psn;

	PVR_DPF_ENTERED;

	PVR_ASSERT(pszName);

	for (psn = psGD->psHead; psn; psn=psn->psNext)
		if (OSStringCompare(psn->psStream->szName, pszName)==0)
			PVR_DPF_RETURN_VAL(psn);

	PVR_DPF_RETURN_VAL(NULL);
}

PTL_SNODE TLFindStreamNodeByDesc(PTL_STREAM_DESC psDesc)
{
	TL_GLOBAL_GDATA* psGD = TLGGD();
	PTL_SNODE 		 psn;

	PVR_DPF_ENTERED;

	PVR_ASSERT(psDesc);

	for (psn = psGD->psHead; psn; psn=psn->psNext)
		if (psn->psDesc == psDesc)
			PVR_DPF_RETURN_VAL(psn);

	PVR_DPF_RETURN_VAL(NULL);
}

IMG_VOID TLTryToRemoveAndFreeStreamNode(PTL_SNODE psRemove)
{
	TL_GLOBAL_GDATA* psGD = TLGGD();
	PTL_SNODE* 		 last;
	PTL_SNODE 		 psn;
	PVRSRV_ERROR     eError;

	PVR_DPF_ENTERED;


	PVR_ASSERT(psRemove);

	// First check is node is really no longer needed
	//
	if (psRemove->psStream != NULL)
		PVR_DPF_RETURN;

	if (psRemove->psDesc != NULL)
	{
		// Signal client if waiting...so they detect stream has been
		// destroyed and can return an error.
		//
		eError = OSEventObjectSignal(psRemove->hDataEventObj);
		if ( eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "OSEventObjectSignal() error %d", eError));
		}

		PVR_DPF_RETURN;
	}

	// Unlink the stream node from the master list
	//
	PVR_ASSERT(psGD->psHead);
	last = &psGD->psHead;
	for (psn = psGD->psHead; psn; psn=psn->psNext)
	{
		if (psn == psRemove)
		{
			*last = psn->psNext;
			break;
		}
		last = &psn->psNext;
	}

	// Release the event list object owned by the stream node
	//
	if (psRemove->hDataEventObj)
	{
		eError = OSEventObjectDestroy(psRemove->hDataEventObj);
		if (eError != PVRSRV_OK)
		{
			PVR_DPF((PVR_DBG_ERROR, "OSEventObjectDestroy() error %d", eError));
		}
	}

	// Release the memory of the stream node
	//
	OSFreeMem(psRemove);

	PVR_DPF_RETURN;
}



