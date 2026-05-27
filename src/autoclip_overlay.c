/*
 * autoclip_overlay.c - Debug overlay for auto-clip region visualization
 *
 * Creates a transparent, click-through, topmost layered window that draws
 * colored outlines around the configured kill feed and badge regions.
 * Used to verify region calibration is correct during gameplay.
 *
 * Drawing approach:
 *   - Kill feed region: green 2px outline
 *   - Badge region: cyan 2px outline, with detected name label
 *   - Updates when player name is detected (redraws with name text)
 */

#include "autoclip_overlay.h"
#include "layered_window.h"
#include "logger.h"
#include <stdio.h>
#include <string.h>

#define REGION_OUTLINE_PX 2

/* Badge outline color: cyan */
#define BD_R 0
#define BD_G 220
#define BD_B 220
#define BD_A 220

/* State */
static struct {
    HWND wnd;
    HINSTANCE hInstance;
    BOOL isVisible;

    /* Monitor-absolute coordinates */
    int monX, monY;       /* Monitor top-left in virtual screen coords */
    int monW, monH;

    /* Badge region rect (relative to monitor top-left) */
    RECT bdRect;

    /* Detected name (shown as label) */
    char playerName[64];

    /* Raw OCR text from badge (for debugging when parse fails) */
    char rawOcr[128];
} g_acOverlay = {0};

static LRESULT CALLBACK AcOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Draw a hollow rectangle outline in the pixel buffer */
static void DrawOutline(BYTE* pixels, int bmpW, int bmpH,
                        int rx, int ry, int rw, int rh,
                        BYTE r, BYTE g, BYTE b, BYTE a)
{
    /* Clamp to bitmap bounds */
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > bmpW) rw = bmpW - rx;
    if (ry + rh > bmpH) rh = bmpH - ry;
    if (rw <= 0 || rh <= 0) return;

    /* Pre-multiplied alpha values */
    BYTE pb = (BYTE)(b * a / 255);
    BYTE pg = (BYTE)(g * a / 255);
    BYTE pr = (BYTE)(r * a / 255);

    for (int t = 0; t < REGION_OUTLINE_PX && t < rh && t < rw; t++) {
        /* Top edge */
        for (int x = rx; x < rx + rw; x++) {
            int idx = ((ry + t) * bmpW + x) * 4;
            pixels[idx + 0] = pb;
            pixels[idx + 1] = pg;
            pixels[idx + 2] = pr;
            pixels[idx + 3] = a;
        }
        /* Bottom edge */
        for (int x = rx; x < rx + rw; x++) {
            int idx = ((ry + rh - 1 - t) * bmpW + x) * 4;
            pixels[idx + 0] = pb;
            pixels[idx + 1] = pg;
            pixels[idx + 2] = pr;
            pixels[idx + 3] = a;
        }
        /* Left edge */
        for (int y = ry; y < ry + rh; y++) {
            int idx = (y * bmpW + rx + t) * 4;
            pixels[idx + 0] = pb;
            pixels[idx + 1] = pg;
            pixels[idx + 2] = pr;
            pixels[idx + 3] = a;
        }
        /* Right edge */
        for (int y = ry; y < ry + rh; y++) {
            int idx = (y * bmpW + rx + rw - 1 - t) * 4;
            pixels[idx + 0] = pb;
            pixels[idx + 1] = pg;
            pixels[idx + 2] = pr;
            pixels[idx + 3] = a;
        }
    }
}

/* Draw text label using GDI on the memory DC (after pixel drawing) */
static void DrawLabel(HDC hdc, int x, int y, const char* text, COLORREF color)
{
    if (!text || text[0] == '\0') return;

    HFONT hFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hOld = (HFONT)SelectObject(hdc, hFont);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);

    /* Draw with slight shadow for readability */
    TextOutA(hdc, x + 1, y + 1, text, (int)strlen(text));
    SetTextColor(hdc, color);
    TextOutA(hdc, x, y, text, (int)strlen(text));

    SelectObject(hdc, hOld);
    DeleteObject(hFont);
}

/* Draw a semi-transparent filled rectangle in the pixel buffer */
static void DrawFilledRect(BYTE* pixels, int bmpW, int bmpH,
                           int rx, int ry, int rw, int rh,
                           BYTE r, BYTE g, BYTE b, BYTE a)
{
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > bmpW) rw = bmpW - rx;
    if (ry + rh > bmpH) rh = bmpH - ry;
    if (rw <= 0 || rh <= 0) return;

    BYTE pb = (BYTE)(b * a / 255);
    BYTE pg = (BYTE)(g * a / 255);
    BYTE pr = (BYTE)(r * a / 255);

    for (int y = ry; y < ry + rh; y++) {
        for (int x = rx; x < rx + rw; x++) {
            int idx = (y * bmpW + x) * 4;
            pixels[idx + 0] = pb;
            pixels[idx + 1] = pg;
            pixels[idx + 2] = pr;
            pixels[idx + 3] = a;
        }
    }
}

static void RedrawOverlay(void)
{
    if (!g_acOverlay.wnd) return;

    int w = g_acOverlay.monW;
    int h = g_acOverlay.monH;
    if (w <= 0 || h <= 0) return;

    LayeredBitmap lb = {0};
    if (!LayeredBitmap_Create(&lb, w, h)) return;

    int bdX = g_acOverlay.bdRect.left;
    int bdY = g_acOverlay.bdRect.top;
    int bdW = g_acOverlay.bdRect.right - g_acOverlay.bdRect.left;
    int bdH = g_acOverlay.bdRect.bottom - g_acOverlay.bdRect.top;

    /* Draw badge region outline (cyan) */
    DrawOutline(lb.pixels, w, h, bdX, bdY, bdW, bdH, BD_R, BD_G, BD_B, BD_A);

    /* Always show a status label above the badge box */
    {
        char label[140];
        COLORREF labelColor;
        int labelY = bdY - 20;
        if (labelY < 0) labelY = g_acOverlay.bdRect.bottom + 4;

        if (g_acOverlay.playerName[0] != '\0') {
            _snprintf(label, sizeof(label), "BADGE: %s", g_acOverlay.playerName);
            labelColor = RGB(0, 255, 100);
        } else if (g_acOverlay.rawOcr[0] != '\0') {
            _snprintf(label, sizeof(label), "BADGE: %s", g_acOverlay.rawOcr);
            labelColor = RGB(255, 200, 0);
        } else {
            _snprintf(label, sizeof(label), "BADGE: (scanning...)");
            labelColor = RGB(0, 220, 220);
        }
        label[sizeof(label) - 1] = '\0';
        DrawLabel(lb.memDC, bdX, labelY, label, labelColor);
    }

    if (g_acOverlay.playerName[0] != '\0') {
        /*
         * Highlight the name inside the badge box.
         * Badge format: "[icon] NAME#1234"
         * The prefix+icon is roughly the left 15%, the #digits is the right 25%.
         * So the name occupies roughly x+15% to x+75% of the badge width.
         * We draw a semi-transparent green fill over that region with 2px inset.
         */
        int inset = 3;
        int nameX = bdX + (int)(bdW * 0.15f) + inset;
        int nameY = bdY + inset;
        int nameW = (int)(bdW * 0.60f);
        int nameH = bdH - (inset * 2);

        /* Green highlight behind the name */
        DrawFilledRect(lb.pixels, w, h, nameX, nameY, nameW, nameH,
                       0, 255, 100, 80);

        /* Bright green outline around the name highlight */
        DrawOutline(lb.pixels, w, h, nameX, nameY, nameW, nameH,
                    0, 255, 100, 200);
    } else if (g_acOverlay.rawOcr[0] != '\0') {
        /*
         * No parsed name yet, but we have raw OCR text.
         * Show in yellow/orange so user can see what OCR is reading.
         */
        int inset = 3;
        int nameX = bdX + inset;
        int nameY = bdY + inset;
        int nameW = bdW - (inset * 2);
        int nameH = bdH - (inset * 2);

        /* Yellow highlight */
        DrawFilledRect(lb.pixels, w, h, nameX, nameY, nameW, nameH,
                       255, 200, 0, 60);

        /* Yellow outline */
        DrawOutline(lb.pixels, w, h, nameX, nameY, nameW, nameH,
                    255, 200, 0, 180);
    }

    LayeredBitmap_Apply(&lb, g_acOverlay.wnd, g_acOverlay.monX, g_acOverlay.monY);
    LayeredBitmap_Destroy(&lb);
}

BOOL AutoClipOverlay_Show(const AppConfig* config, const CaptureState* capture)
{
    if (!config || !capture) return FALSE;

    /* Get monitor position in virtual screen coordinates */
    g_acOverlay.monX = capture->outputDesc.DesktopCoordinates.left;
    g_acOverlay.monY = capture->outputDesc.DesktopCoordinates.top;
    g_acOverlay.monW = capture->monitorWidth;
    g_acOverlay.monH = capture->monitorHeight;

    if (g_acOverlay.monW <= 0 || g_acOverlay.monH <= 0) return FALSE;

    /* Resolve badge percentage to monitor-relative pixel coords */
    g_acOverlay.bdRect.left   = (int)(config->badgeXPct * g_acOverlay.monW);
    g_acOverlay.bdRect.top    = (int)(config->badgeYPct * g_acOverlay.monH);
    g_acOverlay.bdRect.right  = g_acOverlay.bdRect.left + (int)(config->badgeWPct * g_acOverlay.monW);
    g_acOverlay.bdRect.bottom = g_acOverlay.bdRect.top + (int)(config->badgeHPct * g_acOverlay.monH);

    g_acOverlay.playerName[0] = '\0';
    g_acOverlay.rawOcr[0] = '\0';

    /* Register window class (once) */
    HINSTANCE hInst = GetModuleHandle(NULL);
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = AcOverlayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = "LWSRAutoClipOverlay";
    RegisterClassExA(&wc);  /* Ignore if already registered */

    /* Create layered, click-through, topmost window */
    g_acOverlay.wnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "LWSRAutoClipOverlay", NULL, WS_POPUP,
        g_acOverlay.monX, g_acOverlay.monY,
        g_acOverlay.monW, g_acOverlay.monH,
        NULL, NULL, hInst, NULL);

    if (!g_acOverlay.wnd) {
        Logger_Log("AutoClipOverlay: Failed to create window\n");
        return FALSE;
    }

    /* Exclude from screen capture so it doesn't appear in recordings */
    SetWindowDisplayAffinity(g_acOverlay.wnd, WDA_EXCLUDEFROMCAPTURE);

    RedrawOverlay();
    ShowWindow(g_acOverlay.wnd, SW_SHOWNA);
    g_acOverlay.isVisible = TRUE;

    Logger_Log("AutoClipOverlay: Showing name box (Badge: %d,%d %dx%d)\n",
               g_acOverlay.bdRect.left, g_acOverlay.bdRect.top,
               g_acOverlay.bdRect.right - g_acOverlay.bdRect.left,
               g_acOverlay.bdRect.bottom - g_acOverlay.bdRect.top);
    return TRUE;
}

void AutoClipOverlay_SetName(const char* name)
{
    if (!g_acOverlay.wnd) return;

    if (name) {
        strncpy(g_acOverlay.playerName, name, sizeof(g_acOverlay.playerName) - 1);
        g_acOverlay.playerName[sizeof(g_acOverlay.playerName) - 1] = '\0';
    } else {
        g_acOverlay.playerName[0] = '\0';
    }

    RedrawOverlay();
    Logger_Log("AutoClipOverlay: Name updated to '%s'\n",
               g_acOverlay.playerName[0] ? g_acOverlay.playerName : "(none)");
}

void AutoClipOverlay_SetRawOCR(const char* rawText)
{
    if (!g_acOverlay.wnd) return;

    if (rawText) {
        /* Strip newlines/carriage returns for single-line display */
        char clean[128];
        int j = 0;
        for (int i = 0; rawText[i] && j < (int)sizeof(clean) - 1; i++) {
            if (rawText[i] != '\n' && rawText[i] != '\r')
                clean[j++] = rawText[i];
            else if (j > 0 && clean[j-1] != ' ')
                clean[j++] = ' ';
        }
        clean[j] = '\0';
        strncpy(g_acOverlay.rawOcr, clean, sizeof(g_acOverlay.rawOcr) - 1);
        g_acOverlay.rawOcr[sizeof(g_acOverlay.rawOcr) - 1] = '\0';
    } else {
        g_acOverlay.rawOcr[0] = '\0';
    }

    /* Only redraw if no parsed name yet (raw is lower priority) */
    if (g_acOverlay.playerName[0] == '\0') {
        RedrawOverlay();
    }
}

void AutoClipOverlay_Hide(void)
{
    if (g_acOverlay.wnd) {
        DestroyWindow(g_acOverlay.wnd);
        g_acOverlay.wnd = NULL;
    }
    g_acOverlay.isVisible = FALSE;
    g_acOverlay.playerName[0] = '\0';
}
