/*
 * ui_draw.h - Shared UI drawing utilities
 * 
 * GDI+ based anti-aliased drawing functions for overlay UI elements.
 */

#ifndef UI_DRAW_H
#define UI_DRAW_H

#include <windows.h>

/* Convert COLORREF to ARGB format for GDI+ */
DWORD ColorRefToARGB(COLORREF cr, BYTE alpha);

/* Draw anti-aliased filled rounded rectangle */
void DrawRoundedRectAA(HDC hdc, RECT* rect, int radius, COLORREF fillColor, COLORREF borderColor);

/* Draw anti-aliased filled circle */
void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color);

/* Apply smooth rounded corners using DWM (Windows 11+) */
void ApplyRoundedCorners(HWND hwnd);

#endif // UI_DRAW_H
