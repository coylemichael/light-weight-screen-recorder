/*
 * gdiplus_api.h - GDI+ flat API runtime loading
 * 
 * Consolidates GDI+ function pointer loading that was duplicated
 * across overlay.c and action_toolbar.c. Single initialization,
 * shared access via function table struct.
 *
 * Thread Access: Main thread only - UI operations
 */

#ifndef GDIPLUS_API_H
#define GDIPLUS_API_H

#include <windows.h>

/* ============================================================================
 * GDI+ TYPE DEFINITIONS
 * ============================================================================
 * Opaque handles and basic types from GDI+ flat API
 */
typedef int GpStatus;
typedef void* GpGraphics;
typedef void* GpBrush;
typedef void* GpSolidFill;
typedef void* GpPen;
typedef void* GpPath;
typedef void* GpFont;
typedef void* GpFontFamily;
typedef void* GpStringFormat;
typedef float REAL;

/* GDI+ constants */
#define GdipOk                      0
#define SmoothingModeAntiAlias      4
#define TextRenderingHintAntiAlias  4
#define UnitPixel                   2
#define FillModeAlternate           0
#define FontStyleRegular            0
#define FontStyleBold               1
#define StringAlignmentCenter       1

/* ============================================================================
 * GDI+ FUNCTION POINTER TYPES
 * ============================================================================
 */

/* Startup/Shutdown */
typedef struct GdiplusStartupInput {
    UINT32 GdiplusVersion;
    void* DebugEventCallback;
    BOOL SuppressBackgroundThread;
    BOOL SuppressExternalCodecs;
} GdiplusStartupInput;

typedef GpStatus (WINAPI *fn_GdiplusStartup)(ULONG_PTR*, const GdiplusStartupInput*, void*);
typedef void     (WINAPI *fn_GdiplusShutdown)(ULONG_PTR);

/* Graphics */
typedef GpStatus (WINAPI *fn_GdipCreateFromHDC)(HDC, GpGraphics**);
typedef GpStatus (WINAPI *fn_GdipDeleteGraphics)(GpGraphics*);
typedef GpStatus (WINAPI *fn_GdipSetSmoothingMode)(GpGraphics*, int);
typedef GpStatus (WINAPI *fn_GdipSetTextRenderingHint)(GpGraphics*, int);
typedef GpStatus (WINAPI *fn_GdipGraphicsClear)(GpGraphics*, DWORD);

/* Brushes */
typedef GpStatus (WINAPI *fn_GdipCreateSolidFill)(DWORD, GpSolidFill**);
typedef GpStatus (WINAPI *fn_GdipDeleteBrush)(GpBrush*);

/* Pens */
typedef GpStatus (WINAPI *fn_GdipCreatePen1)(DWORD, REAL, int, GpPen**);
typedef GpStatus (WINAPI *fn_GdipDeletePen)(GpPen*);

/* Paths */
typedef GpStatus (WINAPI *fn_GdipCreatePath)(int, GpPath**);
typedef GpStatus (WINAPI *fn_GdipDeletePath)(GpPath*);
typedef GpStatus (WINAPI *fn_GdipAddPathArc)(GpPath*, REAL, REAL, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *fn_GdipAddPathLine)(GpPath*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *fn_GdipClosePathFigure)(GpPath*);
typedef GpStatus (WINAPI *fn_GdipStartPathFigure)(GpPath*);

/* Drawing */
typedef GpStatus (WINAPI *fn_GdipFillRectangle)(GpGraphics*, GpBrush*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *fn_GdipFillEllipse)(GpGraphics*, GpBrush*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *fn_GdipDrawRectangle)(GpGraphics*, GpPen*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *fn_GdipFillPath)(GpGraphics*, GpBrush*, GpPath*);
typedef GpStatus (WINAPI *fn_GdipDrawPath)(GpGraphics*, GpPen*, GpPath*);

/* Fonts and Text */
typedef GpStatus (WINAPI *fn_GdipCreateFontFamilyFromName)(const WCHAR*, void*, GpFontFamily**);
typedef GpStatus (WINAPI *fn_GdipDeleteFontFamily)(GpFontFamily*);
typedef GpStatus (WINAPI *fn_GdipCreateFont)(GpFontFamily*, REAL, int, int, GpFont**);
typedef GpStatus (WINAPI *fn_GdipDeleteFont)(GpFont*);
typedef GpStatus (WINAPI *fn_GdipCreateStringFormat)(int, LANGID, GpStringFormat**);
typedef GpStatus (WINAPI *fn_GdipDeleteStringFormat)(GpStringFormat*);
typedef GpStatus (WINAPI *fn_GdipSetStringFormatAlign)(GpStringFormat*, int);
typedef GpStatus (WINAPI *fn_GdipSetStringFormatLineAlign)(GpStringFormat*, int);
typedef GpStatus (WINAPI *fn_GdipDrawString)(GpGraphics*, const WCHAR*, int, GpFont*, void*, GpStringFormat*, GpBrush*);

/* ============================================================================
 * GDI+ FUNCTION TABLE
 * ============================================================================
 * Consolidates all GDI+ function pointers into a single struct.
 * Reduces 25+ individual globals to one struct instance.
 */
typedef struct GdiplusFunctions {
    /* Module handle */
    HMODULE module;
    ULONG_PTR token;
    BOOL initialized;
    
    /* Startup/Shutdown */
    fn_GdiplusStartup       Startup;
    fn_GdiplusShutdown      Shutdown;
    
    /* Graphics */
    fn_GdipCreateFromHDC            CreateFromHDC;
    fn_GdipDeleteGraphics           DeleteGraphics;
    fn_GdipSetSmoothingMode         SetSmoothingMode;
    fn_GdipSetTextRenderingHint     SetTextRenderingHint;
    fn_GdipGraphicsClear            GraphicsClear;
    
    /* Brushes */
    fn_GdipCreateSolidFill          CreateSolidFill;
    fn_GdipDeleteBrush              BrushDelete;    /* Avoid Windows DeleteBrush macro */
    
    /* Pens */
    fn_GdipCreatePen1               CreatePen1;
    fn_GdipDeletePen                PenDelete;      /* Avoid Windows DeletePen macro */
    
    /* Paths */
    fn_GdipCreatePath               CreatePath;
    fn_GdipDeletePath               DeletePath;
    fn_GdipAddPathArc               AddPathArc;
    fn_GdipAddPathLine              AddPathLine;
    fn_GdipClosePathFigure          ClosePathFigure;
    fn_GdipStartPathFigure          StartPathFigure;
    
    /* Drawing */
    fn_GdipFillRectangle            FillRectangle;
    fn_GdipFillEllipse              FillEllipse;
    fn_GdipDrawRectangle            DrawRectangle;
    fn_GdipFillPath                 FillPath;
    fn_GdipDrawPath                 DrawPath;
    
    /* Fonts and Text */
    fn_GdipCreateFontFamilyFromName CreateFontFamilyFromName;
    fn_GdipDeleteFontFamily         DeleteFontFamily;
    fn_GdipCreateFont               CreateFont;
    fn_GdipDeleteFont               DeleteFont;
    fn_GdipCreateStringFormat       CreateStringFormat;
    fn_GdipDeleteStringFormat       DeleteStringFormat;
    fn_GdipSetStringFormatAlign     SetStringFormatAlign;
    fn_GdipSetStringFormatLineAlign SetStringFormatLineAlign;
    fn_GdipDrawString               DrawString;
} GdiplusFunctions;

/* ============================================================================
 * PUBLIC API
 * ============================================================================
 */

/*
 * GdiplusAPI_Init - Load gdiplus.dll and initialize function pointers
 * Must be called from main thread before any drawing operations.
 * Returns: TRUE on success, FALSE on failure
 */
BOOL GdiplusAPI_Init(GdiplusFunctions* gdi);

/*
 * GdiplusAPI_Shutdown - Release GDI+ resources
 * Must be called from main thread at application exit.
 */
void GdiplusAPI_Shutdown(GdiplusFunctions* gdi);

/*
 * Global shared GDI+ instance
 * Initialized once in main.c, used by overlay.c and action_toolbar.c
 */
extern GdiplusFunctions g_gdip;

#endif /* GDIPLUS_API_H */
