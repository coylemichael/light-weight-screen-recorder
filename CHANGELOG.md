# Changelog

## [0.9.1] - 2025-12-31

### Added
- Stop recording widget with timer display (MM:SS format counting up)
- Click-to-stop functionality on the timer widget
- Subtle hover effect on stop recording button
- Anti-aliased red recording indicator dot using GDI+
- Vertical divider separating timer from "Stop Recording" text

### Fixed
- Timer widget and border now excluded from screen capture (WDA_EXCLUDEFROMCAPTURE)
- All overlay windows created off-screen to prevent capture artifacts
- Improved window positioning to avoid black rectangle artifacts

### Changed
- Modern dark themed stop recording indicator with rounded corners
- Consistent Segoe UI font across all UI elements

## [0.9] - 2025-12-07

### Added
- Initial release
- Area, Window, Monitor, and All Monitors capture modes
- MP4 (H.264), AVI, and WMV output formats
- Quality presets: Low, Medium, High, Lossless
- DXGI Desktop Duplication for hardware-accelerated capture
- Windows 11 Snipping Tool-style UI
- Settings panel with format, quality, time limit, save location
- Single-instance mutex for macro key/Stream Deck toggle support
- Mouse cursor capture option
- Recording border overlay option
- Configurable time limit (hours/minutes/seconds)
- Auto-save with timestamp filenames
- INI-based configuration persistence
