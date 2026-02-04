/*
 * CD Audio Emulation for 16-bit applications
 * Plays WAV files (track02.wav, track03.wav, etc.) instead of real CD audio
 *
 * Copyright 2024 Wine Project Contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __CDAUDIO_EMU_H
#define __CDAUDIO_EMU_H

#include <windows.h>
#include <mmsystem.h>

/* Check if a device type string refers to CD audio */
BOOL CDAUDIO_IsCdAudioDevice(LPCWSTR lpstrDeviceType);
BOOL CDAUDIO_IsCdAudioDeviceA(LPCSTR lpstrDeviceType);

/* Check if a device ID is our emulated CD audio device */
BOOL CDAUDIO_IsEmulatedDevice(MCIDEVICEID wDevID);

/* Handle MCI commands for CD audio - returns TRUE if handled, FALSE to pass through */
BOOL CDAUDIO_HandleCommand(MCIDEVICEID wDevID, UINT wMsg, DWORD dwFlags, DWORD_PTR dwParam, DWORD *pResult);

/* Initialize/cleanup */
void CDAUDIO_Init(void);
void CDAUDIO_Cleanup(void);

#endif /* __CDAUDIO_EMU_H */
