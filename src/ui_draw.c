/*
 * ui_draw.c - Shared UI drawing utilities
 * 
 * GDI+ based anti-aliased drawing functions for overlay UI elements.
 * These are factored out from overlay.c for reuse across UI modules.
 */

#include <windows.h>
#include <dwmapi.h>
#include "ui_draw.h"
#include "gdiplus_api.h"

#pragma comment(lib, "dwmapi.lib")

/* DWM window corner preference constant */
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

DWORD ColorRefToARGB(COLORREF cr, BYTE alpha) {
    return ((DWORD)alpha << 24) | 
           ((DWORD)GetRValue(cr) << 16) | 
           ((DWORD)GetGValue(cr) << 8) | 
           (DWORD)GetBValue(cr);
}

void DrawRoundedRectAA(HDC hdc, RECT* rect, int radius, COLORREF fillColor, COLORREF borderColor) {
    if (!g_gdip.CreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0) return;
    
    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    /* Inset by 0.5 to ensure border is fully visible (GDI+ draws strokes centered on path) */
    float x = (float)rect->left + 0.5f;
    float y = (float)rect->top + 0.5f;
    float w = (float)(rect->right - rect->left) - 1.0f;
    float h = (float)(rect->bottom - rect->top) - 1.0f;
    float r = (float)radius;
    float d = r * 2.0f;
    
    /* Create rounded rectangle path */
    GpPath* path = NULL;
    g_gdip.CreatePath(FillModeAlternate, &path);
    
    /* Top-left arc */
    g_gdip.AddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    /* Top-right arc */
    g_gdip.AddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    /* Bottom-right arc */
    g_gdip.AddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    /* Bottom-left arc */
    g_gdip.AddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    g_gdip.ClosePathFigure(path);
    
    /* Fill */
    GpSolidFill* brush = NULL;
    g_gdip.CreateSolidFill(ColorRefToARGB(fillColor, 255), &brush);
    g_gdip.FillPath(graphics, brush, path);
    g_gdip.BrushDelete(brush);
    
    /* Border */
    GpPen* pen = NULL;
    g_gdip.CreatePen1(ColorRefToARGB(borderColor, 255), 1.0f, UnitPixel, &pen);
    g_gdip.DrawPath(graphics, pen, path);
    g_gdip.PenDelete(pen);
    
    g_gdip.DeletePath(path);
    g_gdip.DeleteGraphics(graphics);
}

void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    if (!g_gdip.CreateFromHDC) return;
    
    GpGraphics* graphics = NULL;
    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0) return;
    
    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);
    
    GpSolidFill* brush = NULL;
    g_gdip.CreateSolidFill(ColorRefToARGB(color, 255), &brush);
    g_gdip.FillEllipse(graphics, brush, 
                    (float)(cx - radius), (float)(cy - radius), 
                    (float)(radius * 2), (float)(radius * 2));
    g_gdip.BrushDelete(brush);
    g_gdip.DeleteGraphics(graphics);
}

void ApplyRoundedCorners(HWND hwnd) {
    DWORD pref = 2;  /* DWMWCP_ROUND */
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}
