/*
 * CD Audio Emulation for 16-bit applications
 * Uses MCI waveaudio to play WAV files instead of real CD audio
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
    MCIDEVICEID wDevID;          /* Our fake cdaudio device ID */
    MCIDEVICEID wWaveDevID;      /* Actual waveaudio device for playback */
    BOOL        bPlaying;
    BOOL        bPaused;
    DWORD       dwCurrentTrack;
    DWORD       dwStartTrack;
    DWORD       dwEndTrack;
    DWORD       dwNumTracks;
    DWORD       dwTimeFormat;
    char        szCdPath[MAX_PATH];  /* Path to look for track files */
} CDAUDIO_STATE;

static CDAUDIO_STATE g_cdState = {0};
static BOOL g_bInitialized = FALSE;

/* Forward declarations */
static BOOL OpenWaveDevice(void);
static void CloseWaveDevice(void);
static BOOL PlayWaveFile(DWORD dwTrack);
static void StopWavePlayback(void);

/***************************************************************************
 * CDAUDIO_Init
 */
void CDAUDIO_Init(void)
{
    if (g_bInitialized) return;

    memset(&g_cdState, 0, sizeof(g_cdState));
    g_cdState.dwTimeFormat = MCI_FORMAT_TMSF;  /* Default: track/min/sec/frame */

    /* Default CD path */
    strcpy(g_cdState.szCdPath, "D:\\");

    /* Assume we have tracks 2-18 (typical game CD layout) */
    g_cdState.dwNumTracks = 18;

    g_bInitialized = TRUE;
    TRACE("CD Audio emulation initialized\n");
}

/***************************************************************************
 * CDAUDIO_Cleanup
 */
void CDAUDIO_Cleanup(void)
{
    if (!g_bInitialized) return;

    CloseWaveDevice();
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
 * OpenWaveDevice - Open an MCI waveaudio device
 */
static BOOL OpenWaveDevice(void)
{
    MCI_OPEN_PARMSA openParms;
    DWORD dwResult;

    if (g_cdState.wWaveDevID != 0)
        return TRUE;  /* Already open */

    memset(&openParms, 0, sizeof(openParms));
    openParms.lpstrDeviceType = "waveaudio";

    dwResult = mciSendCommandA(0, MCI_OPEN, MCI_OPEN_TYPE, (DWORD_PTR)&openParms);
    if (dwResult != 0)
    {
        TRACE("Failed to open waveaudio device: %d\n", dwResult);
        return FALSE;
    }

    g_cdState.wWaveDevID = openParms.wDeviceID;
    TRACE("Opened waveaudio device %d\n", g_cdState.wWaveDevID);
    return TRUE;
}

/***************************************************************************
 * CloseWaveDevice - Close the MCI waveaudio device
 */
static void CloseWaveDevice(void)
{
    if (g_cdState.wWaveDevID != 0)
    {
        mciSendCommandA(g_cdState.wWaveDevID, MCI_CLOSE, 0, 0);
        g_cdState.wWaveDevID = 0;
    }
}

/***************************************************************************
 * PlayWaveFile - Play a track WAV file via MCI waveaudio
 */
static BOOL PlayWaveFile(DWORD dwTrack)
{
    MCI_OPEN_PARMSA openParms;
    MCI_PLAY_PARMS playParms;
    char szPath[MAX_PATH];
    DWORD dwResult;

    if (dwTrack < CDAUDIO_FIRST_AUDIO_TRACK || dwTrack > CDAUDIO_MAX_TRACKS)
        return FALSE;

    /* Close any previous playback */
    CloseWaveDevice();

    /* Build path to track file */
    sprintf(szPath, "%strack%02d.wav", g_cdState.szCdPath, dwTrack);

    TRACE("Playing track %d from %s\n", dwTrack, szPath);

    /* Open the specific WAV file */
    memset(&openParms, 0, sizeof(openParms));
    openParms.lpstrDeviceType = "waveaudio";
    openParms.lpstrElementName = szPath;

    dwResult = mciSendCommandA(0, MCI_OPEN,
                               MCI_OPEN_TYPE | MCI_OPEN_ELEMENT,
                               (DWORD_PTR)&openParms);
    if (dwResult != 0)
    {
        TRACE("Failed to open WAV file %s: error %d\n", szPath, dwResult);
        return FALSE;
    }

    g_cdState.wWaveDevID = openParms.wDeviceID;

    /* Play from beginning */
    memset(&playParms, 0, sizeof(playParms));
    dwResult = mciSendCommandA(g_cdState.wWaveDevID, MCI_PLAY, 0, (DWORD_PTR)&playParms);
    if (dwResult != 0)
    {
        TRACE("Failed to play WAV file: error %d\n", dwResult);
        CloseWaveDevice();
        return FALSE;
    }

    TRACE("Started playback of track %d\n", dwTrack);
    return TRUE;
}

/***************************************************************************
 * StopWavePlayback - Stop current waveaudio playback
 */
static void StopWavePlayback(void)
{
    if (g_cdState.wWaveDevID != 0)
    {
        mciSendCommandA(g_cdState.wWaveDevID, MCI_STOP, 0, 0);
    }
    g_cdState.bPlaying = FALSE;
    g_cdState.bPaused = FALSE;
}

/***************************************************************************
 * HandleOpen - Handle MCI_OPEN for cdaudio
 */
static DWORD HandleOpen(MCIDEVICEID wDevID, DWORD dwFlags, LPMCI_OPEN_PARMSW lpOpenParms)
{
    TRACE("Opening CD audio emulation device %d\n", wDevID);

    if (!g_bInitialized) CDAUDIO_Init();

    if (g_cdState.bOpen)
    {
        return MCIERR_DEVICE_OPEN;
    }

    g_cdState.bOpen = TRUE;
    g_cdState.wDevID = wDevID;
    g_cdState.bPlaying = FALSE;
    g_cdState.bPaused = FALSE;
    g_cdState.dwCurrentTrack = CDAUDIO_FIRST_AUDIO_TRACK;
    g_cdState.dwTimeFormat = MCI_FORMAT_TMSF;

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

    StopWavePlayback();
    CloseWaveDevice();
    g_cdState.bOpen = FALSE;
    g_cdState.wDevID = 0;

    return 0;
}

/***************************************************************************
 * HandlePlay - Handle MCI_PLAY
 */
static DWORD HandlePlay(DWORD dwFlags, LPMCI_PLAY_PARMS lpPlayParms)
{
    DWORD dwFrom = g_cdState.dwCurrentTrack;
    DWORD dwTo = g_cdState.dwNumTracks;

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
    StopWavePlayback();
    CloseWaveDevice();

    /* Start playing the requested track */
    g_cdState.dwCurrentTrack = dwFrom;
    g_cdState.dwStartTrack = dwFrom;
    g_cdState.dwEndTrack = dwTo;

    if (PlayWaveFile(dwFrom))
    {
        g_cdState.bPlaying = TRUE;
        g_cdState.bPaused = FALSE;
    }

    return 0;
}

/***************************************************************************
 * HandleStop - Handle MCI_STOP
 */
static DWORD HandleStop(void)
{
    StopWavePlayback();
    return 0;
}

/***************************************************************************
 * HandlePause - Handle MCI_PAUSE
 */
static DWORD HandlePause(void)
{
    if (g_cdState.bPlaying && !g_cdState.bPaused && g_cdState.wWaveDevID != 0)
    {
        mciSendCommandA(g_cdState.wWaveDevID, MCI_PAUSE, 0, 0);
        g_cdState.bPaused = TRUE;
    }
    return 0;
}

/***************************************************************************
 * HandleResume - Handle MCI_RESUME
 */
static DWORD HandleResume(void)
{
    if (g_cdState.bPaused && g_cdState.wWaveDevID != 0)
    {
        mciSendCommandA(g_cdState.wWaveDevID, MCI_RESUME, 0, 0);
        g_cdState.bPaused = FALSE;
    }
    return 0;
}

/***************************************************************************
 * HandleStatus - Handle MCI_STATUS
 */
static DWORD HandleStatus(DWORD dwFlags, LPMCI_STATUS_PARMS lpStatusParms)
{
    if (!lpStatusParms) return MCIERR_NULL_PARAMETER_BLOCK;

    if (dwFlags & MCI_STATUS_ITEM)
    {
        switch (lpStatusParms->dwItem)
        {
        case MCI_STATUS_LENGTH:
            /* Return a reasonable default length per track (3 minutes) */
            lpStatusParms->dwReturn = 180000;  /* 3 min in ms */
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
            lpStatusParms->dwReturn = TRUE;
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
            TRACE("Unhandled status item %d\n", lpStatusParms->dwItem);
            lpStatusParms->dwReturn = 0;
        }
    }

    return 0;
}

/***************************************************************************
 * HandleSet - Handle MCI_SET
 */
static DWORD HandleSet(DWORD dwFlags, LPMCI_SET_PARMS lpSetParms)
{
    if (!lpSetParms) return MCIERR_NULL_PARAMETER_BLOCK;

    if (dwFlags & MCI_SET_TIME_FORMAT)
    {
        g_cdState.dwTimeFormat = lpSetParms->dwTimeFormat;
        TRACE("Set time format to %d\n", g_cdState.dwTimeFormat);
    }

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
            TRACE("Unhandled getdevcaps item %d\n", lpCapsParms->dwItem);
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
        TRACE("Unhandled MCI command %04x\n", wMsg);
        return FALSE;
    }
}
