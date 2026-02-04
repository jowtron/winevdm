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

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "cdaudio_emu.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(cdaudio_emu);

/* Configuration */
#define CDAUDIO_MAX_TRACKS 99
#define CDAUDIO_FIRST_AUDIO_TRACK 2  /* Track 1 is data, audio starts at track 2 */

/* Emulated CD audio state */
typedef struct {
    BOOL        bOpen;
    MCIDEVICEID wDevID;
    HWAVEOUT    hWaveOut;
    BOOL        bPlaying;
    BOOL        bPaused;
    DWORD       dwCurrentTrack;
    DWORD       dwStartTrack;
    DWORD       dwEndTrack;
    DWORD       dwNumTracks;
    DWORD       dwTimeFormat;
    HANDLE      hPlayThread;
    BOOL        bStopRequested;
    CRITICAL_SECTION cs;
    char        szCdPath[MAX_PATH];  /* Path to look for track files */

    /* Track info */
    struct {
        BOOL    bExists;
        DWORD   dwLengthMS;  /* Length in milliseconds */
    } tracks[CDAUDIO_MAX_TRACKS + 1];

} CDAUDIO_STATE;

static CDAUDIO_STATE g_cdState = {0};
static BOOL g_bInitialized = FALSE;

/* Forward declarations */
static DWORD WINAPI PlayThreadProc(LPVOID lpParam);
static void ScanForTracks(void);
static BOOL PlayTrackFile(DWORD dwTrack);
static void StopPlayback(void);

/***************************************************************************
 * CDAUDIO_Init
 */
void CDAUDIO_Init(void)
{
    if (g_bInitialized) return;

    memset(&g_cdState, 0, sizeof(g_cdState));
    InitializeCriticalSection(&g_cdState.cs);
    g_cdState.dwTimeFormat = MCI_FORMAT_TMSF;  /* Default: track/min/sec/frame */

    /* Default CD path - can be overridden */
    strcpy(g_cdState.szCdPath, "D:\\");

    g_bInitialized = TRUE;
    TRACE("CD Audio emulation initialized\n");
}

/***************************************************************************
 * CDAUDIO_Cleanup
 */
void CDAUDIO_Cleanup(void)
{
    if (!g_bInitialized) return;

    StopPlayback();
    DeleteCriticalSection(&g_cdState.cs);
    g_bInitialized = FALSE;
}

/***************************************************************************
 * CDAUDIO_IsCdAudioDevice - Check if device type is cdaudio
 */
BOOL CDAUDIO_IsCdAudioDevice(LPCWSTR lpstrDeviceType)
{
    if (!lpstrDeviceType) return FALSE;

    /* Check for string "cdaudio" */
    if (_wcsicmp(lpstrDeviceType, L"cdaudio") == 0) return TRUE;

    /* Check for device type ID */
    if (LOWORD(lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO &&
        HIWORD(lpstrDeviceType) == 0) return TRUE;

    return FALSE;
}

BOOL CDAUDIO_IsCdAudioDeviceA(LPCSTR lpstrDeviceType)
{
    if (!lpstrDeviceType) return FALSE;

    /* Check for string "cdaudio" */
    if (_stricmp(lpstrDeviceType, "cdaudio") == 0) return TRUE;

    /* Check for device type ID */
    if (LOWORD((DWORD_PTR)lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO &&
        HIWORD((DWORD_PTR)lpstrDeviceType) == 0) return TRUE;

    return FALSE;
}

/***************************************************************************
 * CDAUDIO_IsEmulatedDevice
 */
BOOL CDAUDIO_IsEmulatedDevice(MCIDEVICEID wDevID)
{
    return (g_cdState.bOpen && g_cdState.wDevID == wDevID);
}

/***************************************************************************
 * ScanForTracks - Find track WAV files on the CD path
 */
static void ScanForTracks(void)
{
    char szPath[MAX_PATH];
    DWORD dwTrack;

    g_cdState.dwNumTracks = 0;

    /* Look for track02.wav through track99.wav */
    for (dwTrack = CDAUDIO_FIRST_AUDIO_TRACK; dwTrack <= CDAUDIO_MAX_TRACKS; dwTrack++)
    {
        sprintf(szPath, "%strack%02d.wav", g_cdState.szCdPath, dwTrack);

        HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);

        if (hFile != INVALID_HANDLE_VALUE)
        {
            /* Get file size to estimate length */
            DWORD dwSize = GetFileSize(hFile, NULL);
            CloseHandle(hFile);

            g_cdState.tracks[dwTrack].bExists = TRUE;
            /* Estimate: 44100 Hz, 16-bit stereo = 176400 bytes/sec */
            g_cdState.tracks[dwTrack].dwLengthMS = (dwSize / 176) ; /* Rough estimate in ms */
            g_cdState.dwNumTracks = dwTrack;

            TRACE("Found track %d: %s (est. %d ms)\n", dwTrack, szPath,
                  g_cdState.tracks[dwTrack].dwLengthMS);
        }
        else
        {
            g_cdState.tracks[dwTrack].bExists = FALSE;
        }
    }

    TRACE("Total tracks found: %d\n", g_cdState.dwNumTracks);
}

/***************************************************************************
 * HandleOpen - Handle MCI_OPEN for cdaudio
 */
static DWORD HandleOpen(MCIDEVICEID wDevID, DWORD dwFlags, LPMCI_OPEN_PARMSW lpOpenParms)
{
    TRACE("Opening CD audio emulation device %d\n", wDevID);

    if (!g_bInitialized) CDAUDIO_Init();

    EnterCriticalSection(&g_cdState.cs);

    if (g_cdState.bOpen)
    {
        LeaveCriticalSection(&g_cdState.cs);
        return MCIERR_DEVICE_OPEN;
    }

    g_cdState.bOpen = TRUE;
    g_cdState.wDevID = wDevID;
    g_cdState.bPlaying = FALSE;
    g_cdState.bPaused = FALSE;
    g_cdState.dwCurrentTrack = CDAUDIO_FIRST_AUDIO_TRACK;
    g_cdState.dwTimeFormat = MCI_FORMAT_TMSF;

    /* Scan for track files */
    ScanForTracks();

    LeaveCriticalSection(&g_cdState.cs);

    /* Update the device ID in the params if provided */
    if (lpOpenParms)
        lpOpenParms->wDeviceID = wDevID;

    return 0;
}

/***************************************************************************
 * HandleClose - Handle MCI_CLOSE
 */
static DWORD HandleClose(void)
{
    TRACE("Closing CD audio emulation\n");

    EnterCriticalSection(&g_cdState.cs);

    StopPlayback();
    g_cdState.bOpen = FALSE;
    g_cdState.wDevID = 0;

    LeaveCriticalSection(&g_cdState.cs);

    return 0;
}

/***************************************************************************
 * PlayTrackFile - Play a specific track WAV file using PlaySound
 */
static BOOL PlayTrackFile(DWORD dwTrack)
{
    char szPath[MAX_PATH];

    if (dwTrack < CDAUDIO_FIRST_AUDIO_TRACK || dwTrack > CDAUDIO_MAX_TRACKS)
        return FALSE;

    if (!g_cdState.tracks[dwTrack].bExists)
        return FALSE;

    sprintf(szPath, "%strack%02d.wav", g_cdState.szCdPath, dwTrack);

    TRACE("Playing track %d from %s\n", dwTrack, szPath);

    /* Use PlaySound for simplicity - plays asynchronously */
    return PlaySoundA(szPath, NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

/***************************************************************************
 * StopPlayback - Stop current playback
 */
static void StopPlayback(void)
{
    if (g_cdState.bPlaying)
    {
        PlaySoundA(NULL, NULL, 0);  /* Stop any playing sound */
        g_cdState.bPlaying = FALSE;
        g_cdState.bPaused = FALSE;
    }
}

/***************************************************************************
 * HandlePlay - Handle MCI_PLAY
 */
static DWORD HandlePlay(DWORD dwFlags, LPMCI_PLAY_PARMS lpPlayParms)
{
    DWORD dwFrom = g_cdState.dwCurrentTrack;
    DWORD dwTo = g_cdState.dwNumTracks;

    EnterCriticalSection(&g_cdState.cs);

    if (lpPlayParms)
    {
        if (dwFlags & MCI_FROM)
        {
            /* Extract track number from TMSF or track format */
            if (g_cdState.dwTimeFormat == MCI_FORMAT_TMSF)
                dwFrom = MCI_TMSF_TRACK(lpPlayParms->dwFrom);
            else
                dwFrom = lpPlayParms->dwFrom;
        }

        if (dwFlags & MCI_TO)
        {
            if (g_cdState.dwTimeFormat == MCI_FORMAT_TMSF)
                dwTo = MCI_TMSF_TRACK(lpPlayParms->dwTo);
            else
                dwTo = lpPlayParms->dwTo;
        }
    }

    TRACE("Play from track %d to %d\n", dwFrom, dwTo);

    /* Stop any current playback */
    StopPlayback();

    /* Start playing the requested track */
    g_cdState.dwCurrentTrack = dwFrom;
    g_cdState.dwStartTrack = dwFrom;
    g_cdState.dwEndTrack = dwTo;

    if (PlayTrackFile(dwFrom))
    {
        g_cdState.bPlaying = TRUE;
        g_cdState.bPaused = FALSE;
    }

    LeaveCriticalSection(&g_cdState.cs);

    return 0;
}

/***************************************************************************
 * HandleStop - Handle MCI_STOP
 */
static DWORD HandleStop(void)
{
    EnterCriticalSection(&g_cdState.cs);
    StopPlayback();
    LeaveCriticalSection(&g_cdState.cs);
    return 0;
}

/***************************************************************************
 * HandlePause - Handle MCI_PAUSE
 */
static DWORD HandlePause(void)
{
    EnterCriticalSection(&g_cdState.cs);

    if (g_cdState.bPlaying && !g_cdState.bPaused)
    {
        /* PlaySound doesn't support pause, so we stop */
        PlaySoundA(NULL, NULL, 0);
        g_cdState.bPaused = TRUE;
    }

    LeaveCriticalSection(&g_cdState.cs);
    return 0;
}

/***************************************************************************
 * HandleResume - Handle MCI_RESUME
 */
static DWORD HandleResume(void)
{
    EnterCriticalSection(&g_cdState.cs);

    if (g_cdState.bPaused)
    {
        /* Resume by replaying current track (not ideal but works) */
        PlayTrackFile(g_cdState.dwCurrentTrack);
        g_cdState.bPaused = FALSE;
    }

    LeaveCriticalSection(&g_cdState.cs);
    return 0;
}

/***************************************************************************
 * HandleStatus - Handle MCI_STATUS
 */
static DWORD HandleStatus(DWORD dwFlags, LPMCI_STATUS_PARMS lpStatusParms)
{
    if (!lpStatusParms) return MCIERR_NULL_PARAMETER_BLOCK;

    EnterCriticalSection(&g_cdState.cs);

    if (dwFlags & MCI_STATUS_ITEM)
    {
        switch (lpStatusParms->dwItem)
        {
        case MCI_STATUS_LENGTH:
            if (dwFlags & MCI_TRACK)
            {
                DWORD dwTrack = lpStatusParms->dwTrack;
                if (dwTrack >= CDAUDIO_FIRST_AUDIO_TRACK &&
                    dwTrack <= CDAUDIO_MAX_TRACKS &&
                    g_cdState.tracks[dwTrack].bExists)
                {
                    /* Return length in current time format */
                    lpStatusParms->dwReturn = g_cdState.tracks[dwTrack].dwLengthMS;
                }
                else
                {
                    lpStatusParms->dwReturn = 0;
                }
            }
            else
            {
                /* Total length */
                DWORD dwTotal = 0;
                for (DWORD i = CDAUDIO_FIRST_AUDIO_TRACK; i <= g_cdState.dwNumTracks; i++)
                {
                    if (g_cdState.tracks[i].bExists)
                        dwTotal += g_cdState.tracks[i].dwLengthMS;
                }
                lpStatusParms->dwReturn = dwTotal;
            }
            break;

        case MCI_STATUS_NUMBER_OF_TRACKS:
            lpStatusParms->dwReturn = g_cdState.dwNumTracks;
            TRACE("Number of tracks: %d\n", g_cdState.dwNumTracks);
            break;

        case MCI_STATUS_MODE:
            if (g_cdState.bPlaying)
                lpStatusParms->dwReturn = g_cdState.bPaused ? MCI_MODE_PAUSE : MCI_MODE_PLAY;
            else
                lpStatusParms->dwReturn = MCI_MODE_STOP;
            break;

        case MCI_STATUS_MEDIA_PRESENT:
            lpStatusParms->dwReturn = (g_cdState.dwNumTracks > 0) ? TRUE : FALSE;
            break;

        case MCI_STATUS_CURRENT_TRACK:
            lpStatusParms->dwReturn = g_cdState.dwCurrentTrack;
            break;

        case MCI_STATUS_POSITION:
            /* Return current position - simplified */
            if (dwFlags & MCI_TRACK)
                lpStatusParms->dwReturn = MCI_MAKE_TMSF(lpStatusParms->dwTrack, 0, 0, 0);
            else
                lpStatusParms->dwReturn = MCI_MAKE_TMSF(g_cdState.dwCurrentTrack, 0, 0, 0);
            break;

        case MCI_STATUS_READY:
            lpStatusParms->dwReturn = TRUE;
            break;

        case MCI_STATUS_TIME_FORMAT:
            lpStatusParms->dwReturn = g_cdState.dwTimeFormat;
            break;

        case MCI_CDA_STATUS_TYPE_TRACK:
            /* All our tracks are audio */
            lpStatusParms->dwReturn = MCI_CDA_TRACK_AUDIO;
            break;

        default:
            FIXME("Unhandled status item %d\n", lpStatusParms->dwItem);
            lpStatusParms->dwReturn = 0;
        }
    }

    LeaveCriticalSection(&g_cdState.cs);

    return 0;
}

/***************************************************************************
 * HandleSet - Handle MCI_SET
 */
static DWORD HandleSet(DWORD dwFlags, LPMCI_SET_PARMS lpSetParms)
{
    if (!lpSetParms) return MCIERR_NULL_PARAMETER_BLOCK;

    EnterCriticalSection(&g_cdState.cs);

    if (dwFlags & MCI_SET_TIME_FORMAT)
    {
        g_cdState.dwTimeFormat = lpSetParms->dwTimeFormat;
        TRACE("Set time format to %d\n", g_cdState.dwTimeFormat);
    }

    LeaveCriticalSection(&g_cdState.cs);

    return 0;
}

/***************************************************************************
 * HandleGetDevCaps - Handle MCI_GETDEVCAPS
 */
static DWORD HandleGetDevCaps(DWORD dwFlags, LPMCI_GETDEVCAPS_PARMS lpCapsParms)
{
    if (!lpCapsParms) return MCIERR_NULL_PARAMETER_BLOCK;

    if (dwFlags & MCI_GETDEVCAPS_ITEM)
    {
        switch (lpCapsParms->dwItem)
        {
        case MCI_GETDEVCAPS_CAN_RECORD:
            lpCapsParms->dwReturn = FALSE;
            break;
        case MCI_GETDEVCAPS_HAS_AUDIO:
            lpCapsParms->dwReturn = TRUE;
            break;
        case MCI_GETDEVCAPS_HAS_VIDEO:
            lpCapsParms->dwReturn = FALSE;
            break;
        case MCI_GETDEVCAPS_DEVICE_TYPE:
            lpCapsParms->dwReturn = MCI_DEVTYPE_CD_AUDIO;
            break;
        case MCI_GETDEVCAPS_USES_FILES:
            lpCapsParms->dwReturn = FALSE;
            break;
        case MCI_GETDEVCAPS_COMPOUND_DEVICE:
            lpCapsParms->dwReturn = FALSE;
            break;
        case MCI_GETDEVCAPS_CAN_EJECT:
            lpCapsParms->dwReturn = FALSE;
            break;
        case MCI_GETDEVCAPS_CAN_PLAY:
            lpCapsParms->dwReturn = TRUE;
            break;
        case MCI_GETDEVCAPS_CAN_SAVE:
            lpCapsParms->dwReturn = FALSE;
            break;
        default:
            FIXME("Unhandled getdevcaps item %d\n", lpCapsParms->dwItem);
            lpCapsParms->dwReturn = 0;
        }
    }

    return 0;
}

/***************************************************************************
 * HandleSeek - Handle MCI_SEEK
 */
static DWORD HandleSeek(DWORD dwFlags, LPMCI_SEEK_PARMS lpSeekParms)
{
    EnterCriticalSection(&g_cdState.cs);

    if (dwFlags & MCI_TO)
    {
        DWORD dwTrack;
        if (g_cdState.dwTimeFormat == MCI_FORMAT_TMSF)
            dwTrack = MCI_TMSF_TRACK(lpSeekParms->dwTo);
        else
            dwTrack = lpSeekParms->dwTo;

        g_cdState.dwCurrentTrack = dwTrack;
        TRACE("Seek to track %d\n", dwTrack);
    }

    LeaveCriticalSection(&g_cdState.cs);

    return 0;
}

/***************************************************************************
 * CDAUDIO_HandleCommand - Main entry point for handling MCI commands
 */
BOOL CDAUDIO_HandleCommand(MCIDEVICEID wDevID, UINT wMsg, DWORD dwFlags,
                           DWORD_PTR dwParam, DWORD *pResult)
{
    /* Initialize if needed */
    if (!g_bInitialized) CDAUDIO_Init();

    /* Check if this is our device (except for OPEN which creates it) */
    if (wMsg != MCI_OPEN && !CDAUDIO_IsEmulatedDevice(wDevID))
        return FALSE;

    TRACE("Handling MCI command %04x for device %d\n", wMsg, wDevID);

    switch (wMsg)
    {
    case MCI_OPEN:
        *pResult = HandleOpen(wDevID, dwFlags, (LPMCI_OPEN_PARMSW)dwParam);
        return TRUE;

    case MCI_CLOSE:
        *pResult = HandleClose();
        return TRUE;

    case MCI_PLAY:
        *pResult = HandlePlay(dwFlags, (LPMCI_PLAY_PARMS)dwParam);
        return TRUE;

    case MCI_STOP:
        *pResult = HandleStop();
        return TRUE;

    case MCI_PAUSE:
        *pResult = HandlePause();
        return TRUE;

    case MCI_RESUME:
        *pResult = HandleResume();
        return TRUE;

    case MCI_STATUS:
        *pResult = HandleStatus(dwFlags, (LPMCI_STATUS_PARMS)dwParam);
        return TRUE;

    case MCI_SET:
        *pResult = HandleSet(dwFlags, (LPMCI_SET_PARMS)dwParam);
        return TRUE;

    case MCI_GETDEVCAPS:
        *pResult = HandleGetDevCaps(dwFlags, (LPMCI_GETDEVCAPS_PARMS)dwParam);
        return TRUE;

    case MCI_SEEK:
        *pResult = HandleSeek(dwFlags, (LPMCI_SEEK_PARMS)dwParam);
        return TRUE;

    case MCI_INFO:
        /* Let the default handler deal with INFO */
        return FALSE;

    default:
        FIXME("Unhandled MCI command %04x\n", wMsg);
        return FALSE;
    }
}
