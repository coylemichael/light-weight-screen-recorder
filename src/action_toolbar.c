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
#include <windowsx.h>  // GET_X_LPARAM, GET_Y_LPARAM
#include "gdiplus_api.h"
#include "layered_window.h"

// windowsx.h defines DeleteFont/SelectFont macros that conflict with GDI+ function names
#undef DeleteFont
#undef SelectFont

/* Additional GDI+ types needed by this module */
typedef DWORD ARGB;

/* ============================================================================
 * TOOLBAR UI STATE
 * ============================================================================
 * Consolidated state for the action toolbar window.
 * Thread Access: [Main thread only - UI operations]
 */

/* Button definitions */
#define BTN_COUNT 5
#define BTN_MINIMIZE 0
#define BTN_RECORD   1
#define BTN_CLOSE    2
#define BTN_SETTINGS 3
#define BTN_UNUSED   4  /* Reserved */

typedef struct {
    RECT rect;
    const WCHAR* text;
    int hovering;
} ToolbarButton;

/*
 * ToolbarUIState - Consolidated toolbar UI state
 */
typedef struct ToolbarUIState {
    HWND wnd;
    HINSTANCE hInstance;
    BOOL initialized;
    ToolbarButton buttons[BTN_COUNT];
    int hoveredButton;
    int pressedButton;
    /* Action callbacks set by the caller */
    void (*onMinimize)(void);
    void (*onRecord)(void);
    void (*onClose)(void);
    void (*onSettings)(void);
} ToolbarUIState;

static ToolbarUIState g_ui = {0};

/* Toolbar dimensions (constants) */
#define TOOLBAR_WIDTH 180
#define TOOLBAR_HEIGHT 36
#define CORNER_RADIUS 10
#define BTN_HEIGHT 24
#define BTN_MARGIN 6
#define BTN_GAP 4

/* Colors (ARGB format for GDI+) */
#define COLOR_BG         0xFF2D2D2D  /* Dark gray background */
#define COLOR_BTN        0xFF3A3A3A  /* Button normal */
#define COLOR_BTN_HOVER  0xFF4A4A4A  // Button hover
#define COLOR_BTN_PRESS  0xFF505050  // Button pressed
#define COLOR_BORDER     0xFF505050  // Border
#define COLOR_TEXT       0xFFFFFFFF  // White text

// Create rounded rectangle path
static GpPath* CreateRoundedRectPath(REAL x, REAL y, REAL w, REAL h, REAL r) {
    GpPath* path = NULL;
    if (g_gdip.CreatePath(0, &path) != 0) return NULL;
    
    // Top-left arc
    g_gdip.AddPathArc(path, x, y, r*2, r*2, 180, 90);
    // Top-right arc
    g_gdip.AddPathArc(path, x + w - r*2, y, r*2, r*2, 270, 90);
    // Bottom-right arc
    g_gdip.AddPathArc(path, x + w - r*2, y + h - r*2, r*2, r*2, 0, 90);
    // Bottom-left arc
    g_gdip.AddPathArc(path, x, y + h - r*2, r*2, r*2, 90, 90);
    g_gdip.ClosePathFigure(path);
    
    return path;
}

// Paint the toolbar to a 32-bit ARGB bitmap
static void PaintToolbar(HDC hdc, int width, int height) {
    GpGraphics* g = NULL;
    if (g_gdip.CreateFromHDC(hdc, &g) != 0) return;
    
    // Enable anti-aliasing
    g_gdip.SetSmoothingMode(g, 4); // SmoothingModeAntiAlias
    g_gdip.SetTextRenderingHint(g, 5); // TextRenderingHintClearTypeGridFit
    
    // Clear to fully transparent
    g_gdip.GraphicsClear(g, 0x00000000);
    
    // Draw background rounded rect
    GpPath* bgPath = CreateRoundedRectPath(0.5f, 0.5f, (REAL)width - 1, (REAL)height - 1, (REAL)CORNER_RADIUS);
    if (bgPath) {
        GpSolidFill* bgBrush = NULL;
        g_gdip.CreateSolidFill(COLOR_BG, &bgBrush);
        if (bgBrush) {
            g_gdip.FillPath(g, bgBrush, bgPath);
            g_gdip.BrushDelete(bgBrush);
        }
        
        // Draw border
        GpPen* borderPen = NULL;
        g_gdip.CreatePen1(COLOR_BORDER, 1.0f, 0, &borderPen);
        if (borderPen) {
            g_gdip.DrawPath(g, borderPen, bgPath);
            g_gdip.PenDelete(borderPen);
        }
        g_gdip.DeletePath(bgPath);
    }
    
    // Draw buttons
    GpFontFamily* fontFamily = NULL;
    GpFont* font = NULL;
    GpStringFormat* format = NULL;
    
    g_gdip.CreateFontFamilyFromName(L"Segoe UI", NULL, &fontFamily);
    if (fontFamily) {
        g_gdip.CreateFont(fontFamily, 11.0f, 0, 2, &font); // 2 = UnitPixel... actually UnitPoint is better
    }
    g_gdip.CreateStringFormat(0, 0, &format);
    if (format) {
        g_gdip.SetStringFormatAlign(format, 1); // StringAlignmentCenter
        g_gdip.SetStringFormatLineAlign(format, 1); // StringAlignmentCenter
    }
    
    for (int i = 0; i < BTN_COUNT; i++) {
        ARGB btnColor = COLOR_BTN;
        if (i == g_ui.pressedButton) btnColor = COLOR_BTN_PRESS;
        else if (i == g_ui.hoveredButton) btnColor = COLOR_BTN_HOVER;
        
        RECT* r = &g_ui.buttons[i].rect;
        GpPath* btnPath = CreateRoundedRectPath((REAL)r->left + 0.5f, (REAL)r->top + 0.5f, 
                                                  (REAL)(r->right - r->left) - 1, 
                                                  (REAL)(r->bottom - r->top) - 1, 4.0f);
        if (btnPath) {
            GpSolidFill* btnBrush = NULL;
            g_gdip.CreateSolidFill(btnColor, &btnBrush);
            if (btnBrush) {
                g_gdip.FillPath(g, btnBrush, btnPath);
                g_gdip.BrushDelete(btnBrush);
            }
            g_gdip.DeletePath(btnPath);
        }
        
        // Draw text
        if (font && format) {
            typedef struct { REAL X, Y, Width, Height; } RectF;
            RectF textRect = { (REAL)r->left, (REAL)r->top, (REAL)(r->right - r->left), (REAL)(r->bottom - r->top) };
            
            GpSolidFill* textBrush = NULL;
            g_gdip.CreateSolidFill(COLOR_TEXT, &textBrush);
            if (textBrush) {
                g_gdip.DrawString(g, g_ui.buttons[i].text, -1, font, &textRect, format, textBrush);
                g_gdip.BrushDelete(textBrush);
            }
        }
    }
    
    if (format) g_gdip.DeleteStringFormat(format);
    if (font) g_gdip.DeleteFont(font);
    if (fontFamily) g_gdip.DeleteFontFamily(fontFamily);
    
    g_gdip.DeleteGraphics(g);
}

// Update the layered window
static void UpdateToolbarBitmap(void) {
    if (!g_ui.wnd) return;
    
    int width = TOOLBAR_WIDTH;
    int height = TOOLBAR_HEIGHT;
    
    LayeredBitmap lb = {0};
    if (!LayeredBitmap_Create(&lb, width, height)) return;
    
    // Paint using GDI+
    PaintToolbar(lb.memDC, width, height);
    
    // Apply and cleanup
    RECT wr;
    GetWindowRect(g_ui.wnd, &wr);
    LayeredBitmap_Apply(&lb, g_ui.wnd, wr.left, wr.top);
    LayeredBitmap_Destroy(&lb);
}

// Hit test to find which button is under the cursor
static int HitTestButton(int x, int y) {
    POINT pt = {x, y};
    for (int i = 0; i < BTN_COUNT; i++) {
        if (PtInRect(&g_ui.buttons[i].rect, pt)) {
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
            
            g_ui.buttons[BTN_MINIMIZE].text = L"\u2014";  // Em dash (horizontal line)
            g_ui.buttons[BTN_RECORD].text = L"\u25CF";   // Filled circle
            g_ui.buttons[BTN_CLOSE].text = L"\u2715";    // Multiplication X
            g_ui.buttons[BTN_SETTINGS].text = L"Settings";
            
            // Layout symbol buttons
            for (int i = 0; i <= BTN_CLOSE; i++) {
                g_ui.buttons[i].rect.left = x;
                g_ui.buttons[i].rect.top = (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;
                g_ui.buttons[i].rect.right = x + smallBtnWidth;
                g_ui.buttons[i].rect.bottom = g_ui.buttons[i].rect.top + BTN_HEIGHT;
                x += smallBtnWidth + btnGap;
            }
            // Settings button is wider
            g_ui.buttons[BTN_SETTINGS].rect.left = x;
            g_ui.buttons[BTN_SETTINGS].rect.top = (TOOLBAR_HEIGHT - BTN_HEIGHT) / 2;
            g_ui.buttons[BTN_SETTINGS].rect.right = x + settingsBtnWidth;
            g_ui.buttons[BTN_SETTINGS].rect.bottom = g_ui.buttons[BTN_SETTINGS].rect.top + BTN_HEIGHT;
            
            // BTN_UNUSED has zero rect (not displayed)
            g_ui.buttons[BTN_UNUSED].rect = (RECT){0, 0, 0, 0};
            return 0;
        }
        
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int btn = HitTestButton(x, y);
            
            if (btn != g_ui.hoveredButton) {
                g_ui.hoveredButton = btn;
                UpdateToolbarBitmap();
            }
            
            // Track mouse leave
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }
        
        case WM_MOUSELEAVE:
            if (g_ui.hoveredButton != -1) {
                g_ui.hoveredButton = -1;
                UpdateToolbarBitmap();
            }
            return 0;
        
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int btn = HitTestButton(x, y);
            if (btn >= 0) {
                g_ui.pressedButton = btn;
                UpdateToolbarBitmap();
                SetCapture(hwnd);
            }
            return 0;
        }
        
        case WM_LBUTTONUP: {
            ReleaseCapture();
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int btn = HitTestButton(x, y);
            
            if (btn >= 0 && btn == g_ui.pressedButton) {
                // Button clicked!
                switch (btn) {
                    case BTN_MINIMIZE: if (g_ui.onMinimize) g_ui.onMinimize(); break;
                    case BTN_RECORD:   if (g_ui.onRecord) g_ui.onRecord(); break;
                    case BTN_CLOSE:    if (g_ui.onClose) g_ui.onClose(); break;
                    case BTN_SETTINGS: if (g_ui.onSettings) g_ui.onSettings(); break;
                }
            }
            
            g_ui.pressedButton = -1;
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
    if (!hInstance) return FALSE;
    if (g_ui.initialized) return TRUE;
    
    g_ui.hInstance = hInstance;
    
    // GDI+ is now initialized globally via g_gdip in main.c
    if (!g_gdip.initialized) return FALSE;
    
    // Register window class
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ToolbarWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "LWSRActionToolbar";
    
    if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return FALSE;
    }
    
    // Create layered window
    g_ui.wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        "LWSRActionToolbar",
        NULL,
        WS_POPUP,
        -9999, -9999, TOOLBAR_WIDTH, TOOLBAR_HEIGHT,  // Start off-screen
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_ui.wnd) return FALSE;
    
    g_ui.initialized = TRUE;
    return TRUE;
}

void ActionToolbar_Shutdown(void) {
    if (g_ui.wnd) {
        DestroyWindow(g_ui.wnd);
        g_ui.wnd = NULL;
    }
    // GDI+ shutdown handled globally by main.c
    g_ui.initialized = FALSE;
}

void ActionToolbar_Show(int x, int y) {
    if (!g_ui.wnd) return;
    
    SetWindowPos(g_ui.wnd, HWND_TOPMOST, 
                 x - TOOLBAR_WIDTH / 2, y,
                 TOOLBAR_WIDTH, TOOLBAR_HEIGHT, 
                 SWP_NOACTIVATE);
    
    UpdateToolbarBitmap();
    ShowWindow(g_ui.wnd, SW_SHOWNOACTIVATE);
}

void ActionToolbar_Hide(void) {
    if (g_ui.wnd) {
        ShowWindow(g_ui.wnd, SW_HIDE);
    }
}

void ActionToolbar_SetCallbacks(void (*onMinimize)(void), void (*onRecord)(void), 
                                 void (*onClose)(void), void (*onSettings)(void)) {
    g_ui.onMinimize = onMinimize;
    g_ui.onRecord = onRecord;
    g_ui.onClose = onClose;
    g_ui.onSettings = onSettings;
}
