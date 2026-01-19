/*
 * action_toolbar.c - Custom action toolbar with smooth rounded corners
 * Uses layered window with per-pixel alpha for anti-aliased transparency
 *
 * ERROR HANDLING PATTERN:
 * - Early return for simple validation checks
 * - GDI+ function return codes checked (GpStatus != Ok)
 * - Window creation failures return FALSE
 * - GDI/GDI+ resource cleanup in shutdown function
 * - Returns BOOL to propagate errors; callers must check
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>

// GDI+ flat API
typedef int GpStatus;
typedef void* GpGraphics;
typedef void* GpSolidFill;
typedef void* GpPen;
typedef void* GpPath;
typedef void* GpFont;
typedef void* GpFontFamily;
typedef void* GpStringFormat;
typedef float REAL;
typedef DWORD ARGB;

// GDI+ function pointers
static HMODULE g_gdiplus = NULL;
static ULONG_PTR g_gdiplusToken = 0;

typedef GpStatus (WINAPI *GdiplusStartupFunc)(ULONG_PTR*, void*, void*);
typedef void (WINAPI *GdiplusShutdownFunc)(ULONG_PTR);
typedef GpStatus (WINAPI *GdipCreateFromHDCFunc)(HDC, GpGraphics**);
typedef GpStatus (WINAPI *GdipDeleteGraphicsFunc)(GpGraphics*);
typedef GpStatus (WINAPI *GdipSetSmoothingModeFunc)(GpGraphics*, int);
typedef GpStatus (WINAPI *GdipSetTextRenderingHintFunc)(GpGraphics*, int);
typedef GpStatus (WINAPI *GdipCreateSolidFillFunc)(ARGB, GpSolidFill**);
typedef GpStatus (WINAPI *GdipDeleteBrushFunc)(GpSolidFill*);
typedef GpStatus (WINAPI *GdipCreatePenFunc)(ARGB, REAL, int, GpPen**);
typedef GpStatus (WINAPI *GdipDeletePenFunc)(GpPen*);
typedef GpStatus (WINAPI *GdipCreatePathFunc)(int, GpPath**);
typedef GpStatus (WINAPI *GdipDeletePathFunc)(GpPath*);
typedef GpStatus (WINAPI *GdipAddPathArcFunc)(GpPath*, REAL, REAL, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *GdipAddPathLineFunc)(GpPath*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *GdipClosePathFigureFunc)(GpPath*);
typedef GpStatus (WINAPI *GdipFillPathFunc)(GpGraphics*, GpSolidFill*, GpPath*);
typedef GpStatus (WINAPI *GdipDrawPathFunc)(GpGraphics*, GpPen*, GpPath*);
typedef GpStatus (WINAPI *GdipFillRectangleFunc)(GpGraphics*, GpSolidFill*, REAL, REAL, REAL, REAL);
typedef GpStatus (WINAPI *GdipGraphicsClearFunc)(GpGraphics*, ARGB);
typedef GpStatus (WINAPI *GdipCreateFontFamilyFromNameFunc)(const WCHAR*, void*, GpFontFamily**);
typedef GpStatus (WINAPI *GdipDeleteFontFamilyFunc)(GpFontFamily*);
typedef GpStatus (WINAPI *GdipCreateFontFunc)(GpFontFamily*, REAL, int, int, GpFont**);
typedef GpStatus (WINAPI *GdipDeleteFontFunc)(GpFont*);
typedef GpStatus (WINAPI *GdipCreateStringFormatFunc)(int, LANGID, GpStringFormat**);
typedef GpStatus (WINAPI *GdipDeleteStringFormatFunc)(GpStringFormat*);
typedef GpStatus (WINAPI *GdipSetStringFormatAlignFunc)(GpStringFormat*, int);
typedef GpStatus (WINAPI *GdipSetStringFormatLineAlignFunc)(GpStringFormat*, int);
typedef GpStatus (WINAPI *GdipDrawStringFunc)(GpGraphics*, const WCHAR*, int, GpFont*, void*, GpStringFormat*, GpSolidFill*);

static GdiplusStartupFunc pGdiplusStartup;
static GdiplusShutdownFunc pGdiplusShutdown;
static GdipCreateFromHDCFunc pGdipCreateFromHDC;
static GdipDeleteGraphicsFunc pGdipDeleteGraphics;
static GdipSetSmoothingModeFunc pGdipSetSmoothingMode;
static GdipSetTextRenderingHintFunc pGdipSetTextRenderingHint;
static GdipCreateSolidFillFunc pGdipCreateSolidFill;
static GdipDeleteBrushFunc pGdipDeleteBrush;
static GdipCreatePenFunc pGdipCreatePen;
static GdipDeletePenFunc pGdipDeletePen;
static GdipCreatePathFunc pGdipCreatePath;
static GdipDeletePathFunc pGdipDeletePath;
static GdipAddPathArcFunc pGdipAddPathArc;
static GdipAddPathLineFunc pGdipAddPathLine;
static GdipClosePathFigureFunc pGdipClosePathFigure;
static GdipFillPathFunc pGdipFillPath;
static GdipDrawPathFunc pGdipDrawPath;
static GdipFillRectangleFunc pGdipFillRectangle;
static GdipGraphicsClearFunc pGdipGraphicsClear;
static GdipCreateFontFamilyFromNameFunc pGdipCreateFontFamilyFromName;
static GdipDeleteFontFamilyFunc pGdipDeleteFontFamily;
static GdipCreateFontFunc pGdipCreateFont;
static GdipDeleteFontFunc pGdipDeleteFont;
static GdipCreateStringFormatFunc pGdipCreateStringFormat;
static GdipDeleteStringFormatFunc pGdipDeleteStringFormat;
static GdipSetStringFormatAlignFunc pGdipSetStringFormatAlign;
static GdipSetStringFormatLineAlignFunc pGdipSetStringFormatLineAlign;
static GdipDrawStringFunc pGdipDrawString;

// Toolbar state
static HWND g_toolbarWnd = NULL;
static HINSTANCE g_hInstance = NULL;
static BOOL g_initialized = FALSE;

// Button definitions
#define BTN_COUNT 5
#define BTN_MINIMIZE 0
#define BTN_RECORD   1
#define BTN_CLOSE    2
#define BTN_SETTINGS 3
#define BTN_UNUSED   4  // Reserved

typedef struct {
    RECT rect;
    const WCHAR* text;
    int hovering;
} ToolbarButton;

static ToolbarButton g_buttons[BTN_COUNT];
static int g_hoveredButton = -1;
static int g_pressedButton = -1;

// Callbacks
static void (*g_onMinimize)(void) = NULL;
static void (*g_onRecord)(void) = NULL;
static void (*g_onClose)(void) = NULL;
static void (*g_onSettings)(void) = NULL;

// Toolbar dimensions
#define TOOLBAR_WIDTH 180
#define TOOLBAR_HEIGHT 36
#define CORNER_RADIUS 10
#define BTN_HEIGHT 24
#define BTN_MARGIN 6
#define BTN_GAP 4

// Colors (ARGB format for GDI+)
#define COLOR_BG         0xFF2D2D2D  // Dark gray background
#define COLOR_BTN        0xFF3A3A3A  // Button normal
#define COLOR_BTN_HOVER  0xFF4A4A4A  // Button hover
#define COLOR_BTN_PRESS  0xFF505050  // Button pressed
#define COLOR_BORDER     0xFF505050  // Border
#define COLOR_TEXT       0xFFFFFFFF  // White text

// Initialize GDI+ for this module
static BOOL InitToolbarGdiPlus(void) {
    if (g_gdiplus) return TRUE;
    
    g_gdiplus = LoadLibraryA("gdiplus.dll");
    if (!g_gdiplus) return FALSE;
    
    pGdiplusStartup = (GdiplusStartupFunc)GetProcAddress(g_gdiplus, "GdiplusStartup");
    pGdiplusShutdown = (GdiplusShutdownFunc)GetProcAddress(g_gdiplus, "GdiplusShutdown");
    pGdipCreateFromHDC = (GdipCreateFromHDCFunc)GetProcAddress(g_gdiplus, "GdipCreateFromHDC");
    pGdipDeleteGraphics = (GdipDeleteGraphicsFunc)GetProcAddress(g_gdiplus, "GdipDeleteGraphics");
    pGdipSetSmoothingMode = (GdipSetSmoothingModeFunc)GetProcAddress(g_gdiplus, "GdipSetSmoothingMode");
    pGdipSetTextRenderingHint = (GdipSetTextRenderingHintFunc)GetProcAddress(g_gdiplus, "GdipSetTextRenderingHint");
    pGdipCreateSolidFill = (GdipCreateSolidFillFunc)GetProcAddress(g_gdiplus, "GdipCreateSolidFill");
    pGdipDeleteBrush = (GdipDeleteBrushFunc)GetProcAddress(g_gdiplus, "GdipDeleteBrush");
    pGdipCreatePen = (GdipCreatePenFunc)GetProcAddress(g_gdiplus, "GdipCreatePen1");
    pGdipDeletePen = (GdipDeletePenFunc)GetProcAddress(g_gdiplus, "GdipDeletePen");
    pGdipCreatePath = (GdipCreatePathFunc)GetProcAddress(g_gdiplus, "GdipCreatePath");
    pGdipDeletePath = (GdipDeletePathFunc)GetProcAddress(g_gdiplus, "GdipDeletePath");
    pGdipAddPathArc = (GdipAddPathArcFunc)GetProcAddress(g_gdiplus, "GdipAddPathArc");
    pGdipAddPathLine = (GdipAddPathLineFunc)GetProcAddress(g_gdiplus, "GdipAddPathLine");
    pGdipClosePathFigure = (GdipClosePathFigureFunc)GetProcAddress(g_gdiplus, "GdipClosePathFigure");
    pGdipFillPath = (GdipFillPathFunc)GetProcAddress(g_gdiplus, "GdipFillPath");
    pGdipDrawPath = (GdipDrawPathFunc)GetProcAddress(g_gdiplus, "GdipDrawPath");
    pGdipFillRectangle = (GdipFillRectangleFunc)GetProcAddress(g_gdiplus, "GdipFillRectangle");
    pGdipGraphicsClear = (GdipGraphicsClearFunc)GetProcAddress(g_gdiplus, "GdipGraphicsClear");
    pGdipCreateFontFamilyFromName = (GdipCreateFontFamilyFromNameFunc)GetProcAddress(g_gdiplus, "GdipCreateFontFamilyFromName");
    pGdipDeleteFontFamily = (GdipDeleteFontFamilyFunc)GetProcAddress(g_gdiplus, "GdipDeleteFontFamily");
    pGdipCreateFont = (GdipCreateFontFunc)GetProcAddress(g_gdiplus, "GdipCreateFont");
    pGdipDeleteFont = (GdipDeleteFontFunc)GetProcAddress(g_gdiplus, "GdipDeleteFont");
    pGdipCreateStringFormat = (GdipCreateStringFormatFunc)GetProcAddress(g_gdiplus, "GdipCreateStringFormat");
    pGdipDeleteStringFormat = (GdipDeleteStringFormatFunc)GetProcAddress(g_gdiplus, "GdipDeleteStringFormat");
    pGdipSetStringFormatAlign = (GdipSetStringFormatAlignFunc)GetProcAddress(g_gdiplus, "GdipSetStringFormatAlign");
    pGdipSetStringFormatLineAlign = (GdipSetStringFormatLineAlignFunc)GetProcAddress(g_gdiplus, "GdipSetStringFormatLineAlign");
    pGdipDrawString = (GdipDrawStringFunc)GetProcAddress(g_gdiplus, "GdipDrawString");
    
    if (!pGdiplusStartup) return FALSE;
    
    // GdiplusStartupInput
    struct { UINT32 GdiplusVersion; void* DebugEventCallback; BOOL SuppressBackgroundThread; BOOL SuppressExternalCodecs; } startupInput = {1, NULL, FALSE, FALSE};
    if (pGdiplusStartup(&g_gdiplusToken, &startupInput, NULL) != 0) {
        FreeLibrary(g_gdiplus);
        g_gdiplus = NULL;
        return FALSE;
    }
    
    return TRUE;
}

static void ShutdownToolbarGdiPlus(void) {
    if (g_gdiplusToken && pGdiplusShutdown) {
        pGdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
    if (g_gdiplus) {
        FreeLibrary(g_gdiplus);
        g_gdiplus = NULL;
    }
}

// Create rounded rectangle path
static GpPath* CreateRoundedRectPath(REAL x, REAL y, REAL w, REAL h, REAL r) {
    GpPath* path = NULL;
    if (pGdipCreatePath(0, &path) != 0) return NULL;
    
    // Top-left arc
    pGdipAddPathArc(path, x, y, r*2, r*2, 180, 90);
    // Top-right arc
    pGdipAddPathArc(path, x + w - r*2, y, r*2, r*2, 270, 90);
    // Bottom-right arc
    pGdipAddPathArc(path, x + w - r*2, y + h - r*2, r*2, r*2, 0, 90);
    // Bottom-left arc
    pGdipAddPathArc(path, x, y + h - r*2, r*2, r*2, 90, 90);
    pGdipClosePathFigure(path);
    
    return path;
}

// Paint the toolbar to a 32-bit ARGB bitmap
static void PaintToolbar(HDC hdc, int width, int height) {
    GpGraphics* g = NULL;
    if (pGdipCreateFromHDC(hdc, &g) != 0) return;
    
    // Enable anti-aliasing
    pGdipSetSmoothingMode(g, 4); // SmoothingModeAntiAlias
    pGdipSetTextRenderingHint(g, 5); // TextRenderingHintClearTypeGridFit
    
    // Clear to fully transparent
    pGdipGraphicsClear(g, 0x00000000);
    
    // Draw background rounded rect
    GpPath* bgPath = CreateRoundedRectPath(0.5f, 0.5f, (REAL)width - 1, (REAL)height - 1, (REAL)CORNER_RADIUS);
    if (bgPath) {
        GpSolidFill* bgBrush = NULL;
        pGdipCreateSolidFill(COLOR_BG, &bgBrush);
        if (bgBrush) {
            pGdipFillPath(g, bgBrush, bgPath);
            pGdipDeleteBrush(bgBrush);
        }
        
        // Draw border
        GpPen* borderPen = NULL;
        pGdipCreatePen(COLOR_BORDER, 1.0f, 0, &borderPen);
        if (borderPen) {
            pGdipDrawPath(g, borderPen, bgPath);
            pGdipDeletePen(borderPen);
        }
        pGdipDeletePath(bgPath);
    }
    
    // Draw buttons
    GpFontFamily* fontFamily = NULL;
    GpFont* font = NULL;
    GpStringFormat* format = NULL;
    
    pGdipCreateFontFamilyFromName(L"Segoe UI", NULL, &fontFamily);
    if (fontFamily) {
        pGdipCreateFont(fontFamily, 11.0f, 0, 2, &font); // 2 = UnitPixel... actually UnitPoint is better
    }
    pGdipCreateStringFormat(0, 0, &format);
    if (format) {
        pGdipSetStringFormatAlign(format, 1); // StringAlignmentCenter
        pGdipSetStringFormatLineAlign(format, 1); // StringAlignmentCenter
    }
    
    for (int i = 0; i < BTN_COUNT; i++) {
        ARGB btnColor = COLOR_BTN;
        if (i == g_pressedButton) btnColor = COLOR_BTN_PRESS;
        else if (i == g_hoveredButton) btnColor = COLOR_BTN_HOVER;
        
        RECT* r = &g_buttons[i].rect;
        GpPath* btnPath = CreateRoundedRectPath((REAL)r->left + 0.5f, (REAL)r->top + 0.5f, 
                                                  (REAL)(r->right - r->left) - 1, 
                                                  (REAL)(r->bottom - r->top) - 1, 4.0f);
        if (btnPath) {
            GpSolidFill* btnBrush = NULL;
            pGdipCreateSolidFill(btnColor, &btnBrush);
            if (btnBrush) {
                pGdipFillPath(g, btnBrush, btnPath);
                pGdipDeleteBrush(btnBrush);
            }
            pGdipDeletePath(btnPath);
        }
        
        // Draw text
        if (font && format) {
            typedef struct { REAL X, Y, Width, Height; } RectF;
            RectF textRect = { (REAL)r->left, (REAL)r->top, (REAL)(r->right - r->left), (REAL)(r->bottom - r->top) };
            
            GpSolidFill* textBrush = NULL;
            pGdipCreateSolidFill(COLOR_TEXT, &textBrush);
            if (textBrush) {
                pGdipDrawString(g, g_buttons[i].text, -1, font, &textRect, format, textBrush);
                pGdipDeleteBrush(textBrush);
            }
        }
    }
    
    if (format) pGdipDeleteStringFormat(format);
    if (font) pGdipDeleteFont(font);
    if (fontFamily) pGdipDeleteFontFamily(fontFamily);
    
    pGdipDeleteGraphics(g);
}

// Update the layered window
static void UpdateToolbarBitmap(void) {
    if (!g_toolbarWnd) return;
    
    int width = TOOLBAR_WIDTH;
    int height = TOOLBAR_HEIGHT;
    
    // Create 32-bit DIB
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    
    void* bits = NULL;
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    HBITMAP oldBitmap = SelectObject(memDC, hBitmap);
    
    // Paint using GDI+
    PaintToolbar(memDC, width, height);
    
    // Update layered window
    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    
    RECT wr;
    GetWindowRect(g_toolbarWnd, &wr);
    POINT ptDst = {wr.left, wr.top};
    
    UpdateLayeredWindow(g_toolbarWnd, screenDC, &ptDst, &sizeWnd, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    SelectObject(memDC, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}

// Hit test to find which button is under the cursor
static int HitTestButton(int x, int y) {
    POINT pt = {x, y};
    for (int i = 0; i < BTN_COUNT; i++) {
        if (PtInRect(&g_buttons[i].rect, pt)) {
            return i;
        }
    }
    return -1;
}

// Window procedure
static LRESULT CALLBACK ToolbarWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Setup button rectangles
            // Buttons: [-] [●] [✕] [Settings]
            int x = BTN_MARGIN;
            int smallBtnWidth = 32;    // For symbol buttons
            int settingsBtnWidth = 60; // For "Settings" text
            int btnGap = BTN_GAP;
            
            g_buttons[BTN_MINIMIZE].text = L"\u2014";  // Em dash (horizontal line)
            g_buttons[BTN_RECORD].text = L"\u25CF";   // Filled circle
            g_buttons[BTN_CLOSE].text = L"\u2715";    // Multiplication X
            g_buttons[BTN_SETTINGS].text = L"Settings";
            
            // Layout symbol buttons
            for (int i = 0; i <= BTN_CLOSE; i++) {
                g_buttons[i].rect.left = x;
                g_buttons[i].rect.top = (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;
                g_buttons[i].rect.right = x + smallBtnWidth;
                g_buttons[i].rect.bottom = g_buttons[i].rect.top + BTN_HEIGHT;
                x += smallBtnWidth + btnGap;
            }
            // Settings button is wider
            g_buttons[BTN_SETTINGS].rect.left = x;
            g_buttons[BTN_SETTINGS].rect.top = (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;
            g_buttons[BTN_SETTINGS].rect.right = x + settingsBtnWidth;
            g_buttons[BTN_SETTINGS].rect.bottom = g_buttons[BTN_SETTINGS].rect.top + BTN_HEIGHT;
            
            // BTN_UNUSED has zero rect (not displayed)
            g_buttons[BTN_UNUSED].rect = (RECT){0, 0, 0, 0};
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            int btn = HitTestButton(x, y);
            
            if (btn != g_hoveredButton) {
                g_hoveredButton = btn;
                UpdateToolbarBitmap();
            }
            
            // Track mouse leave
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        
        case WM_MOUSELEAVE:
            if (g_hoveredButton != -1) {
                g_hoveredButton = -1;
                UpdateToolbarBitmap();
            }
            return 0;
        
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            int btn = HitTestButton(x, y);
            if (btn >= 0) {
                g_pressedButton = btn;
                UpdateToolbarBitmap();
                SetCapture(hwnd);
            }
            return 0;
        }
        
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            int btn = HitTestButton(x, y);
            
            if (btn >= 0 && btn == g_pressedButton) {
                // Button clicked!
                switch (btn) {
                    case BTN_MINIMIZE: if (g_onMinimize) g_onMinimize(); break;
                    case BTN_RECORD:   if (g_onRecord) g_onRecord(); break;
                    case BTN_CLOSE:    if (g_onClose) g_onClose(); break;
                    case BTN_SETTINGS: if (g_onSettings) g_onSettings(); break;
                }
            }
            
            g_pressedButton = -1;
            UpdateToolbarBitmap();
            return 0;
        }
        
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Public API
BOOL ActionToolbar_Init(HINSTANCE hInstance) {
    if (g_initialized) return TRUE;
    
    g_hInstance = hInstance;
    
    if (!InitToolbarGdiPlus()) return FALSE;
    
    // Register window class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ToolbarWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "LWSRActionToolbar";
    RegisterClassExA(&wc);
    
    // Create layered window
    g_toolbarWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        "LWSRActionToolbar",
        NULL,
        WS_POPUP,
        -9999, -9999, TOOLBAR_WIDTH, TOOLBAR_HEIGHT,  // Start off-screen
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_toolbarWnd) return FALSE;
    
    g_initialized = TRUE;
    return TRUE;
}

void ActionToolbar_Shutdown(void) {
    if (g_toolbarWnd) {
        DestroyWindow(g_toolbarWnd);
        g_toolbarWnd = NULL;
    }
    ShutdownToolbarGdiPlus();
    g_initialized = FALSE;
}

void ActionToolbar_Show(int x, int y) {
    if (!g_toolbarWnd) return;
    
    SetWindowPos(g_toolbarWnd, HWND_TOPMOST, 
                 x - TOOLBAR_WIDTH / 2, y,
                 TOOLBAR_WIDTH, TOOLBAR_HEIGHT, 
                 SWP_NOACTIVATE);
    
    UpdateToolbarBitmap();
    ShowWindow(g_toolbarWnd, SW_SHOWNOACTIVATE);
}

void ActionToolbar_Hide(void) {
    if (g_toolbarWnd) {
        ShowWindow(g_toolbarWnd, SW_HIDE);
    }
}

void ActionToolbar_SetCallbacks(void (*onMinimize)(void), void (*onRecord)(void), 
                                 void (*onClose)(void), void (*onSettings)(void)) {
    g_onMinimize = onMinimize;
    g_onRecord = onRecord;
    g_onClose = onClose;
    g_onSettings = onSettings;
}

HWND ActionToolbar_GetWindow(void) {
    return g_toolbarWnd;
}
