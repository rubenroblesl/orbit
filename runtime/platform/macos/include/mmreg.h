// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <windows.h>

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IEEE_FLOAT 3u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu

#pragma pack(push, 1)
struct WAVEFORMATEX {
    WORD wFormatTag;
    WORD nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD nBlockAlign;
    WORD wBitsPerSample;
    WORD cbSize;
};
#pragma pack(pop)
using LPWAVEFORMATEX = WAVEFORMATEX*;
