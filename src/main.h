/*
 * main.h - Extern declarations for globals defined in main.c
 */

#ifndef MAIN_H
#define MAIN_H

#include <windows.h>
#include "config.h"
#include "capture.h"
#include "replay_buffer.h"

/* Globals defined in main.c - thread access documented there */
extern AppConfig g_config;
extern CaptureState g_capture;
extern ReplayBufferState g_replayBuffer;
extern volatile LONG g_isRecording;
extern volatile LONG g_isSelecting;
extern HWND g_overlayWnd;
extern HWND g_controlWnd;
extern HANDLE g_mutex;

#endif /* MAIN_H */
