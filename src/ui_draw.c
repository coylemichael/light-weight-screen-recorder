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

DWORD ColorRefToARGB(COLORREF cr) {
    return (DWORD)0xFF000000 |
           ((DWORD)GetRValue(cr) << 16) |
           ((DWORD)GetGValue(cr) << 8) |
           (DWORD)GetBValue(cr);
}

void DrawRoundedRectAA(HDC hdc, RECT* rect, int radius, COLORREF fillColor, COLORREF borderColor) {
    /* Single CreateFromHDC check suffices: gdiplus_api.c loads all g_gdip members as a
     * batch and fails closed, so either all pointers are valid or none are. */
    if (!g_gdip.CreateFromHDC) return;
    if (!rect || radius <= 0) return;

    int rw = rect->right - rect->left;
    int rh = rect->bottom - rect->top;
    if (rw < 2 * radius || rh < 2 * radius) return;

    GpGraphics* graphics = NULL;
    GpPath* path = NULL;
    GpSolidFill* brush = NULL;
    GpPen* pen = NULL;

    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0 || !graphics) {
        graphics = NULL;
        goto cleanup;
    }

    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);

    /* Inset by 0.5 to ensure border is fully visible (GDI+ draws strokes centered on path) */
    float x = (float)rect->left + 0.5f;
    float y = (float)rect->top + 0.5f;
    float w = (float)rw - 1.0f;
    float h = (float)rh - 1.0f;
    float r = (float)radius;
    float d = r * 2.0f;

    if (g_gdip.CreatePath(FillModeAlternate, &path) != 0 || !path) {
        path = NULL;
        goto cleanup;
    }

    /* Top-left arc */
    g_gdip.AddPathArc(path, x, y, d, d, 180.0f, 90.0f);
    /* Top-right arc */
    g_gdip.AddPathArc(path, x + w - d, y, d, d, 270.0f, 90.0f);
    /* Bottom-right arc */
    g_gdip.AddPathArc(path, x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    /* Bottom-left arc */
    g_gdip.AddPathArc(path, x, y + h - d, d, d, 90.0f, 90.0f);
    g_gdip.ClosePathFigure(path);

    if (g_gdip.CreateSolidFill(ColorRefToARGB(fillColor), &brush) != 0 || !brush) {
        brush = NULL;
        goto cleanup;
    }
    g_gdip.FillPath(graphics, brush, path);

    if (g_gdip.CreatePen1(ColorRefToARGB(borderColor), 1.0f, UnitPixel, &pen) != 0 || !pen) {
        pen = NULL;
        goto cleanup;
    }
    g_gdip.DrawPath(graphics, pen, path);

cleanup:
    if (pen) g_gdip.PenDelete(pen);
    if (brush) g_gdip.BrushDelete(brush);
    if (path) g_gdip.DeletePath(path);
    if (graphics) g_gdip.DeleteGraphics(graphics);
}

void DrawCircleAA(HDC hdc, int cx, int cy, int radius, COLORREF color) {
    /* See DrawRoundedRectAA: single CreateFromHDC check suffices for the whole batch. */
    if (!g_gdip.CreateFromHDC) return;
    if (radius <= 0) return;

    GpGraphics* graphics = NULL;
    GpSolidFill* brush = NULL;

    if (g_gdip.CreateFromHDC(hdc, &graphics) != 0 || !graphics) {
        graphics = NULL;
        goto cleanup;
    }

    g_gdip.SetSmoothingMode(graphics, SmoothingModeAntiAlias);

    if (g_gdip.CreateSolidFill(ColorRefToARGB(color), &brush) != 0 || !brush) {
        brush = NULL;
        goto cleanup;
    }
    g_gdip.FillEllipse(graphics, brush,
                    (float)(cx - radius), (float)(cy - radius),
                    (float)(radius * 2), (float)(radius * 2));

cleanup:
    if (brush) g_gdip.BrushDelete(brush);
    if (graphics) g_gdip.DeleteGraphics(graphics);
}

void ApplyRoundedCorners(HWND hwnd) {
    DWORD pref = 2;  /* DWMWCP_ROUND */
    /* Silent fallback on pre-Win11 is intentional. */
    (void)DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}
