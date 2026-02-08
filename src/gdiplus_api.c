/*
 * gdiplus_api.c - GDI+ flat API runtime loading implementation
 * 
 * Consolidates GDI+ function pointer loading that was duplicated
 * across overlay.c and action_toolbar.c.
 */

#include "gdiplus_api.h"
#include "logger.h"

/* Global shared GDI+ instance */
GdiplusFunctions g_gdip = {0};

/* Helper macro for loading functions */
#define LOAD_GDIP_FUNC(name) \
    gdi->name = (fn_Gdip##name)GetProcAddress(gdi->module, "Gdip" #name); \
    if (!gdi->name) { \
        Logger_Log("GDI+: Failed to load Gdip" #name "\n"); \
        failed = TRUE; \
    }

BOOL GdiplusAPI_Init(GdiplusFunctions* gdi) {
    if (!gdi) return FALSE;
    if (gdi->initialized) return TRUE;
    
    /* Load gdiplus.dll */
    gdi->module = LoadLibraryW(L"gdiplus.dll");
    if (!gdi->module) {
        Logger_Log("GDI+: Failed to load gdiplus.dll\n");
        return FALSE;
    }
    
    BOOL failed = FALSE;
    
    /* Load startup/shutdown - these have special names */
    gdi->Startup = (fn_GdiplusStartup)GetProcAddress(gdi->module, "GdiplusStartup");
    gdi->Shutdown = (fn_GdiplusShutdown)GetProcAddress(gdi->module, "GdiplusShutdown");
    if (!gdi->Startup || !gdi->Shutdown) {
        Logger_Log("GDI+: Failed to load startup/shutdown functions\n");
        FreeLibrary(gdi->module);
        gdi->module = NULL;
        return FALSE;
    }
    
    /* Load all other functions */
    LOAD_GDIP_FUNC(CreateFromHDC);
    LOAD_GDIP_FUNC(DeleteGraphics);
    LOAD_GDIP_FUNC(SetSmoothingMode);
    LOAD_GDIP_FUNC(GraphicsClear);
    LOAD_GDIP_FUNC(CreateSolidFill);
    /* Load with explicit names to avoid Windows macro conflict */
    gdi->BrushDelete = (fn_GdipDeleteBrush)GetProcAddress(gdi->module, "GdipDeleteBrush");
    if (!gdi->BrushDelete) { Logger_Log("GDI+: Failed to load GdipDeleteBrush\n"); failed = TRUE; }
    gdi->PenDelete = (fn_GdipDeletePen)GetProcAddress(gdi->module, "GdipDeletePen");
    if (!gdi->PenDelete) { Logger_Log("GDI+: Failed to load GdipDeletePen\n"); failed = TRUE; }
    LOAD_GDIP_FUNC(CreatePath);
    LOAD_GDIP_FUNC(DeletePath);
    LOAD_GDIP_FUNC(AddPathArc);
    LOAD_GDIP_FUNC(ClosePathFigure);
    LOAD_GDIP_FUNC(FillEllipse);
    LOAD_GDIP_FUNC(FillPath);
    LOAD_GDIP_FUNC(DrawPath);
    LOAD_GDIP_FUNC(CreateFontFamilyFromName);
    LOAD_GDIP_FUNC(DeleteFontFamily);
    LOAD_GDIP_FUNC(CreateFont);
    LOAD_GDIP_FUNC(DeleteFont);
    LOAD_GDIP_FUNC(CreateStringFormat);
    LOAD_GDIP_FUNC(DeleteStringFormat);
    LOAD_GDIP_FUNC(SetStringFormatAlign);
    LOAD_GDIP_FUNC(SetStringFormatLineAlign);
    LOAD_GDIP_FUNC(DrawString);
    
    /* These have slightly different names - load manually */
    gdi->SetTextRenderingHint = (fn_GdipSetTextRenderingHint)GetProcAddress(gdi->module, "GdipSetTextRenderingHint");
    if (!gdi->SetTextRenderingHint) { Logger_Log("GDI+: Failed to load GdipSetTextRenderingHint\n"); failed = TRUE; }
    gdi->CreatePen1 = (fn_GdipCreatePen1)GetProcAddress(gdi->module, "GdipCreatePen1");
    if (!gdi->CreatePen1) { Logger_Log("GDI+: Failed to load GdipCreatePen1\n"); failed = TRUE; }
    
    if (failed) {
        Logger_Log("GDI+: Some functions failed to load\n");
        FreeLibrary(gdi->module);
        gdi->module = NULL;
        return FALSE;
    }
    
    /* Initialize GDI+ */
    GdiplusStartupInput input = {0};
    input.GdiplusVersion = 1;
    
    GpStatus status = gdi->Startup(&gdi->token, &input, NULL);
    if (status != GdipOk) {
        Logger_Log("GDI+: GdiplusStartup failed with status %d\n", status);
        FreeLibrary(gdi->module);
        gdi->module = NULL;
        return FALSE;
    }
    
    gdi->initialized = TRUE;
    Logger_Log("GDI+: Initialized successfully\n");
    return TRUE;
}

void GdiplusAPI_Shutdown(GdiplusFunctions* gdi) {
    if (!gdi || !gdi->initialized) return;
    
    if (gdi->Shutdown && gdi->token) {
        gdi->Shutdown(gdi->token);
        gdi->token = 0;
    }
    
    if (gdi->module) {
        FreeLibrary(gdi->module);
        gdi->module = NULL;
    }
    
    gdi->initialized = FALSE;
    Logger_Log("GDI+: Shutdown complete\n");
}
