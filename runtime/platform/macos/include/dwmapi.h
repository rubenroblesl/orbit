// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Desktop Window Manager timing. macOS has no DWM; the runtime loads these through
// GetProcAddress(dwmapi.dll), which the compat loader refuses, so its fallbacks apply.

#include <windows.h>

using DWM_FRAME_COUNT = ULONGLONG;
using QPC_TIME = ULONGLONG;
struct UNSIGNED_RATIO {
    UINT32 uiNumerator;
    UINT32 uiDenominator;
};
#pragma pack(push, 1)
struct DWM_TIMING_INFO {
    UINT32 cbSize;
    UNSIGNED_RATIO rateRefresh;
    QPC_TIME qpcRefreshPeriod;
    UNSIGNED_RATIO rateCompose;
    QPC_TIME qpcVBlank;
    DWM_FRAME_COUNT cRefresh;
    UINT cDXRefresh;
    QPC_TIME qpcCompose;
    DWM_FRAME_COUNT cFrame;
    UINT cDXPresent;
    DWM_FRAME_COUNT cRefreshFrame;
    DWM_FRAME_COUNT cFrameSubmitted;
    UINT cDXPresentSubmitted;
    DWM_FRAME_COUNT cFrameConfirmed;
    UINT cDXPresentConfirmed;
    DWM_FRAME_COUNT cRefreshConfirmed;
    UINT cDXRefreshConfirmed;
    DWM_FRAME_COUNT cFramesLate;
    UINT cFramesOutstanding;
    DWM_FRAME_COUNT cFrameDisplayed;
    QPC_TIME qpcFrameDisplayed;
    DWM_FRAME_COUNT cRefreshFrameDisplayed;
    DWM_FRAME_COUNT cFrameComplete;
    QPC_TIME qpcFrameComplete;
    DWM_FRAME_COUNT cFramePending;
    QPC_TIME qpcFramePending;
    DWM_FRAME_COUNT cFramesDisplayed;
    DWM_FRAME_COUNT cFramesComplete;
    DWM_FRAME_COUNT cFramesPending;
    DWM_FRAME_COUNT cFramesAvailable;
    DWM_FRAME_COUNT cFramesDropped;
    DWM_FRAME_COUNT cFramesMissed;
    DWM_FRAME_COUNT cRefreshNextDisplayed;
    DWM_FRAME_COUNT cRefreshNextPresented;
    DWM_FRAME_COUNT cRefreshesDisplayed;
    DWM_FRAME_COUNT cRefreshesPresented;
    DWM_FRAME_COUNT cRefreshStarted;
    ULONGLONG cPixelsReceived;
    ULONGLONG cPixelsDrawn;
    DWM_FRAME_COUNT cBuffersEmpty;
};
#pragma pack(pop)
