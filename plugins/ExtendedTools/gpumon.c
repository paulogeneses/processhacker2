/*
 * Process Hacker Extended Tools -
 *   GPU monitoring
 *
 * Copyright (C) 2011 wj32
 *
 * This file is part of Process Hacker.
 *
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "exttools.h"
#include "setupapi.h"
#include "d3dkmt.h"
#include "gpumon.h"

static GUID GUID_DISPLAY_DEVICE_ARRIVAL_I = { 0x1ca05180, 0xa699, 0x450a, { 0x9a, 0x0c, 0xde, 0x4f, 0xbe, 0x3d, 0xdd, 0x89 } };

static PFND3DKMT_OPENADAPTERFROMDEVICENAME D3DKMTOpenAdapterFromDeviceName_I;
static PFND3DKMT_CLOSEADAPTER D3DKMTCloseAdapter_I;
static PFND3DKMT_QUERYSTATISTICS D3DKMTQueryStatistics_I;
static _SetupDiGetClassDevsW SetupDiGetClassDevsW_I;
static _SetupDiDestroyDeviceInfoList SetupDiDestroyDeviceInfoList_I;
static _SetupDiEnumDeviceInterfaces SetupDiEnumDeviceInterfaces_I;
static _SetupDiGetDeviceInterfaceDetailW SetupDiGetDeviceInterfaceDetailW_I;

BOOLEAN EtGpuEnabled;
static PPH_LIST EtpGpuAdapterList;
static PH_CALLBACK_REGISTRATION ProcessesUpdatedCallbackRegistration;

ULONG EtGpuTotalNodeCount;
ULONG EtGpuTotalSegmentCount;
ULONG64 EtGpuDedicatedLimit;
ULONG64 EtGpuSharedLimit;

PH_UINT64_DELTA EtClockTotalRunningTimeDelta;
LARGE_INTEGER EtClockTotalRunningTimeFrequency;
PH_UINT64_DELTA EtGpuTotalRunningTimeDelta;
PH_UINT64_DELTA EtGpuSystemRunningTimeDelta;
FLOAT EtGpuNodeUsage;
PH_CIRCULAR_BUFFER_FLOAT EtGpuNodeHistory;
PH_CIRCULAR_BUFFER_ULONG EtMaxGpuNodeHistory; // ID of max. GPU usage process

ULONG64 EtGpuDedicatedUsage;
ULONG64 EtGpuSharedUsage;
PH_CIRCULAR_BUFFER_ULONG EtGpuDedicatedHistory;
PH_CIRCULAR_BUFFER_ULONG EtGpuSharedHistory;

VOID EtGpuMonitorInitialization(
    VOID
    )
{
    if (PhGetIntegerSetting(SETTING_NAME_ENABLE_GPU_MONITOR))
    {
        HMODULE gdi32Handle;
        HMODULE setupapiHandle;

        if (gdi32Handle = GetModuleHandle(L"gdi32.dll"))
        {
            D3DKMTOpenAdapterFromDeviceName_I = (PVOID)GetProcAddress(gdi32Handle, "D3DKMTOpenAdapterFromDeviceName");
            D3DKMTCloseAdapter_I = (PVOID)GetProcAddress(gdi32Handle, "D3DKMTCloseAdapter");
            D3DKMTQueryStatistics_I = (PVOID)GetProcAddress(gdi32Handle, "D3DKMTQueryStatistics");
        }

        if (setupapiHandle = LoadLibrary(L"setupapi.dll"))
        {
            SetupDiGetClassDevsW_I = (PVOID)GetProcAddress(setupapiHandle, "SetupDiGetClassDevsW");
            SetupDiDestroyDeviceInfoList_I = (PVOID)GetProcAddress(setupapiHandle, "SetupDiDestroyDeviceInfoList");
            SetupDiEnumDeviceInterfaces_I = (PVOID)GetProcAddress(setupapiHandle, "SetupDiEnumDeviceInterfaces");
            SetupDiGetDeviceInterfaceDetailW_I = (PVOID)GetProcAddress(setupapiHandle, "SetupDiGetDeviceInterfaceDetailW");
        }

        if (
            D3DKMTOpenAdapterFromDeviceName_I &&
            D3DKMTCloseAdapter_I &&
            D3DKMTQueryStatistics_I &&
            SetupDiGetClassDevsW_I &&
            SetupDiDestroyDeviceInfoList_I &&
            SetupDiEnumDeviceInterfaces_I &&
            SetupDiGetDeviceInterfaceDetailW_I
            )
        {
            EtpGpuAdapterList = PhCreateList(4);

            if (EtpInitializeD3DStatistics() && EtpGpuAdapterList->Count != 0)
                EtGpuEnabled = TRUE;
        }
    }

    if (EtGpuEnabled)
    {
        ULONG sampleCount;

        sampleCount = PhGetIntegerSetting(L"SampleCount");
        PhInitializeCircularBuffer_FLOAT(&EtGpuNodeHistory, sampleCount);
        PhInitializeCircularBuffer_ULONG(&EtMaxGpuNodeHistory, sampleCount);
        PhInitializeCircularBuffer_ULONG(&EtGpuDedicatedHistory, sampleCount);
        PhInitializeCircularBuffer_ULONG(&EtGpuSharedHistory, sampleCount);

        PhRegisterCallback(
            &PhProcessesUpdatedEvent,
            ProcessesUpdatedCallback,
            NULL,
            &ProcessesUpdatedCallbackRegistration
            );
    }
}

static BOOLEAN EtpInitializeD3DStatistics(
    VOID
    )
{
    LOGICAL result;
    HDEVINFO deviceInfoSet;
    ULONG memberIndex;
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA detailData;
    ULONG detailDataSize;
    D3DKMT_OPENADAPTERFROMDEVICENAME openAdapterFromDeviceName;
    D3DKMT_QUERYSTATISTICS queryStatistics;

    deviceInfoSet = SetupDiGetClassDevsW_I(&GUID_DISPLAY_DEVICE_ARRIVAL_I, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);

    if (!deviceInfoSet)
        return FALSE;

    memberIndex = 0;
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    while (SetupDiEnumDeviceInterfaces_I(deviceInfoSet, NULL, &GUID_DISPLAY_DEVICE_ARRIVAL_I, memberIndex, &deviceInterfaceData))
    {
        detailDataSize = 0x100;
        detailData = PhAllocate(detailDataSize);
        detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!(result = SetupDiGetDeviceInterfaceDetailW_I(deviceInfoSet, &deviceInterfaceData, detailData, detailDataSize, &detailDataSize, NULL)) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            PhFree(detailData);
            detailData = PhAllocate(detailDataSize);

            if (detailDataSize >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA))
                detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

            result = SetupDiGetDeviceInterfaceDetailW_I(deviceInfoSet, &deviceInterfaceData, detailData, detailDataSize, &detailDataSize, NULL);
        }

        if (result)
        {
            openAdapterFromDeviceName.pDeviceName = detailData->DevicePath;

            if (NT_SUCCESS(D3DKMTOpenAdapterFromDeviceName_I(&openAdapterFromDeviceName)))
            {
                memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));
                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
                queryStatistics.AdapterLuid = openAdapterFromDeviceName.AdapterLuid;

                if (NT_SUCCESS(D3DKMTQueryStatistics_I(&queryStatistics)))
                {
                    PETP_GPU_ADAPTER gpuAdapter;
                    ULONG i;

                    gpuAdapter = PhAllocate(sizeof(ETP_GPU_ADAPTER));
                    gpuAdapter->AdapterLuid = openAdapterFromDeviceName.AdapterLuid;
                    gpuAdapter->NodeCount = queryStatistics.QueryResult.AdapterInformation.NodeCount;
                    gpuAdapter->SegmentCount = queryStatistics.QueryResult.AdapterInformation.NbSegments;
                    gpuAdapter->ApertureBitMap = 0;

                    PhAddItemList(EtpGpuAdapterList, gpuAdapter);
                    EtGpuTotalNodeCount += gpuAdapter->NodeCount;
                    EtGpuTotalSegmentCount += gpuAdapter->SegmentCount;

                    for (i = 0; i < gpuAdapter->SegmentCount; i++)
                    {
                        memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));
                        queryStatistics.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
                        queryStatistics.AdapterLuid = gpuAdapter->AdapterLuid;
                        queryStatistics.QuerySegment.SegmentId = i;

                        if (NT_SUCCESS(D3DKMTQueryStatistics_I(&queryStatistics)))
                        {
                            if (queryStatistics.QueryResult.SegmentInformationV1.Aperture)
                                EtGpuSharedLimit += queryStatistics.QueryResult.SegmentInformationV1.CommitLimit;
                            else
                                EtGpuDedicatedLimit += queryStatistics.QueryResult.SegmentInformationV1.CommitLimit;

                            if (i < 32)
                            {
                                if (queryStatistics.QueryResult.SegmentInformationV1.Aperture)
                                    gpuAdapter->ApertureBitMap |= 1 << i;
                            }
                        }
                    }
                }
            }
        }

        PhFree(detailData);

        memberIndex++;
    }

    SetupDiDestroyDeviceInfoList_I(deviceInfoSet);

    return TRUE;
}

static VOID EtpUpdateSegmentInformation(
    __in_opt PET_PROCESS_BLOCK Block
    )
{
    ULONG i;
    ULONG j;
    PETP_GPU_ADAPTER gpuAdapter;
    D3DKMT_QUERYSTATISTICS queryStatistics;
    ULONG64 dedicatedUsage;
    ULONG64 sharedUsage;

    if (Block && !Block->ProcessItem->QueryHandle)
        return;

    dedicatedUsage = 0;
    sharedUsage = 0;

    for (i = 0; i < EtpGpuAdapterList->Count; i++)
    {
        gpuAdapter = EtpGpuAdapterList->Items[i];

        for (j = 0; j < gpuAdapter->SegmentCount; j++)
        {
            memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

            if (Block)
                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT;
            else
                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;

            queryStatistics.AdapterLuid = gpuAdapter->AdapterLuid;

            if (Block)
            {
                queryStatistics.hProcess = Block->ProcessItem->QueryHandle;
                queryStatistics.QueryProcessSegment.SegmentId = j;
            }
            else
            {
                queryStatistics.QuerySegment.SegmentId = j;
            }

            if (NT_SUCCESS(D3DKMTQueryStatistics_I(&queryStatistics)))
            {
                if (Block)
                {
                    if (gpuAdapter->ApertureBitMap & (1 << j))
                        sharedUsage += queryStatistics.QueryResult.ProcessSegmentInformation.BytesCommitted;
                    else
                        dedicatedUsage += queryStatistics.QueryResult.ProcessSegmentInformation.BytesCommitted;
                }
                else
                {
                    if (gpuAdapter->ApertureBitMap & (1 << j))
                        sharedUsage += queryStatistics.QueryResult.SegmentInformationV1.BytesCommitted;
                    else
                        dedicatedUsage += queryStatistics.QueryResult.SegmentInformationV1.BytesCommitted;
                }
            }
        }
    }

    if (Block)
    {
        Block->GpuDedicatedUsage = dedicatedUsage;
        Block->GpuSharedUsage = sharedUsage;
    }
    else
    {
        EtGpuDedicatedUsage = dedicatedUsage;
        EtGpuSharedUsage = sharedUsage;
    }
}

static VOID EtpUpdateNodeInformation(
    __in_opt PET_PROCESS_BLOCK Block
    )
{
    ULONG i;
    ULONG j;
    PETP_GPU_ADAPTER gpuAdapter;
    D3DKMT_QUERYSTATISTICS queryStatistics;
    ULONG64 totalRunningTime;
    ULONG64 systemRunningTime;

    if (Block && !Block->ProcessItem->QueryHandle)
        return;

    totalRunningTime = 0;
    systemRunningTime = 0;

    for (i = 0; i < EtpGpuAdapterList->Count; i++)
    {
        gpuAdapter = EtpGpuAdapterList->Items[i];

        for (j = 0; j < gpuAdapter->NodeCount; j++)
        {
            memset(&queryStatistics, 0, sizeof(D3DKMT_QUERYSTATISTICS));

            if (Block)
                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
            else
                queryStatistics.Type = D3DKMT_QUERYSTATISTICS_NODE;

            queryStatistics.AdapterLuid = gpuAdapter->AdapterLuid;

            if (Block)
            {
                queryStatistics.hProcess = Block->ProcessItem->QueryHandle;
                queryStatistics.QueryProcessNode.NodeId = j;
            }
            else
            {
                queryStatistics.QueryNode.NodeId = j;
            }

            if (NT_SUCCESS(D3DKMTQueryStatistics_I(&queryStatistics)))
            {
                if (Block)
                {
                    totalRunningTime += queryStatistics.QueryResult.ProcessNodeInformation.RunningTime.QuadPart;
                }
                else
                {
                    totalRunningTime += queryStatistics.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart;
                    systemRunningTime += queryStatistics.QueryResult.NodeInformation.SystemInformation.RunningTime.QuadPart;
                }
            }
        }
    }

    if (Block)
    {
        PhUpdateDelta(&Block->GpuRunningTimeDelta, totalRunningTime);
    }
    else
    {
        LARGE_INTEGER performanceCounter;

        NtQueryPerformanceCounter(&performanceCounter, &EtClockTotalRunningTimeFrequency);
        PhUpdateDelta(&EtClockTotalRunningTimeDelta, performanceCounter.QuadPart);
        PhUpdateDelta(&EtGpuTotalRunningTimeDelta, totalRunningTime);
        PhUpdateDelta(&EtGpuSystemRunningTimeDelta, systemRunningTime);
    }
}

static VOID NTAPI ProcessesUpdatedCallback(
    __in_opt PVOID Parameter,
    __in_opt PVOID Context
    )
{
    static ULONG runCount = 0; // MUST keep in sync with runCount in process provider

    ULONG64 elapsedTime; // total GPU node elapsed time in micro-seconds
    PLIST_ENTRY listEntry;
    FLOAT maxNodeValue = 0;
    PET_PROCESS_BLOCK maxNodeBlock = NULL;

    // Update global statistics.

    EtpUpdateSegmentInformation(NULL);
    EtpUpdateNodeInformation(NULL);
    elapsedTime = EtClockTotalRunningTimeDelta.Delta * 1000000 * EtGpuTotalNodeCount / EtClockTotalRunningTimeFrequency.QuadPart;

    EtGpuNodeUsage = (FLOAT)EtGpuTotalRunningTimeDelta.Delta / elapsedTime;

    // Update per-process statistics.
    // Note: no lock is needed because we only ever modify the list on this same thread.

    listEntry = EtProcessBlockListHead.Flink;

    while (listEntry != &EtProcessBlockListHead)
    {
        PET_PROCESS_BLOCK block;

        block = CONTAINING_RECORD(listEntry, ET_PROCESS_BLOCK, ListEntry);

        EtpUpdateSegmentInformation(block);
        EtpUpdateNodeInformation(block);

        if (elapsedTime != 0)
            block->GpuNodeUsage = (FLOAT)block->GpuRunningTimeDelta.Delta / elapsedTime;

        if (maxNodeValue < block->GpuNodeUsage)
        {
            maxNodeValue = block->GpuNodeUsage;
            maxNodeBlock = block;
        }

        listEntry = listEntry->Flink;
    }

    // Update history buffers.

    if (runCount != 0)
    {
        PhAddItemCircularBuffer_FLOAT(&EtGpuNodeHistory, EtGpuNodeUsage);
        PhAddItemCircularBuffer_ULONG(&EtGpuDedicatedHistory, (ULONG)(EtGpuDedicatedUsage / PAGE_SIZE));
        PhAddItemCircularBuffer_ULONG(&EtGpuSharedHistory, (ULONG)(EtGpuSharedUsage / PAGE_SIZE));

        if (maxNodeBlock)
        {
            PhAddItemCircularBuffer_ULONG(&EtMaxGpuNodeHistory, (ULONG)maxNodeBlock->ProcessItem->ProcessId);
            PhReferenceProcessRecordForStatistics(maxNodeBlock->ProcessItem->Record);
        }
        else
        {
            PhAddItemCircularBuffer_ULONG(&EtMaxGpuNodeHistory, (ULONG)NULL);
        }
    }

    runCount++;
}