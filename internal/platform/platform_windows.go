//go:build windows

package platform

import (
	"context"
	"encoding/binary"
	"fmt"
	"image"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unicode/utf16"
	"unsafe"
)

var (
	user32                   = syscall.NewLazyDLL("user32.dll")
	gdi32                    = syscall.NewLazyDLL("gdi32.dll")
	kernel32                 = syscall.NewLazyDLL("kernel32.dll")
	getSystemMetrics         = user32.NewProc("GetSystemMetrics")
	getDC                    = user32.NewProc("GetDC")
	releaseDC                = user32.NewProc("ReleaseDC")
	createCompatibleDC       = gdi32.NewProc("CreateCompatibleDC")
	createCompatibleBitmap   = gdi32.NewProc("CreateCompatibleBitmap")
	selectObject             = gdi32.NewProc("SelectObject")
	bitBlt                   = gdi32.NewProc("BitBlt")
	getDIBits                = gdi32.NewProc("GetDIBits")
	deleteObject             = gdi32.NewProc("DeleteObject")
	deleteDC                 = gdi32.NewProc("DeleteDC")
	setCursorPos             = user32.NewProc("SetCursorPos")
	mouseEvent               = user32.NewProc("mouse_event")
	sendInput                = user32.NewProc("SendInput")
	openClipboard            = user32.NewProc("OpenClipboard")
	closeClipboard           = user32.NewProc("CloseClipboard")
	emptyClipboard           = user32.NewProc("EmptyClipboard")
	getClipboardData         = user32.NewProc("GetClipboardData")
	setClipboardData         = user32.NewProc("SetClipboardData")
	globalAlloc              = kernel32.NewProc("GlobalAlloc")
	globalLock               = kernel32.NewProc("GlobalLock")
	globalUnlock             = kernel32.NewProc("GlobalUnlock")
	globalFree               = kernel32.NewProc("GlobalFree")
	globalSize               = kernel32.NewProc("GlobalSize")
	getCurrentProcess        = kernel32.NewProc("GetCurrentProcess")
	readProcessMemory        = kernel32.NewProc("ReadProcessMemory")
	writeProcessMemory       = kernel32.NewProc("WriteProcessMemory")
	enumWindows              = user32.NewProc("EnumWindows")
	isWindowVisible          = user32.NewProc("IsWindowVisible")
	getWindowText            = user32.NewProc("GetWindowTextW")
	getForegroundWindow      = user32.NewProc("GetForegroundWindow")
	getWindowThreadProcessID = user32.NewProc("GetWindowThreadProcessId")
	getGUIThreadInfo         = user32.NewProc("GetGUIThreadInfo")
	isChild                  = user32.NewProc("IsChild")
	showWindow               = user32.NewProc("ShowWindow")
	setForegroundWindow      = user32.NewProc("SetForegroundWindow")
	shellExecute             = user32.NewProc("ShellExecuteW")
)

const (
	srccopy       = 0x00CC0020
	biRGB         = 0
	cfUnicodeText = 13
	gmemMoveable  = 0x0002
	keyUp         = 0x0002
	keyUnicode    = 0x0004
)

type bitmapInfoHeader struct {
	Size          uint32
	Width         int32
	Height        int32
	Planes        uint16
	BitCount      uint16
	Compression   uint32
	SizeImage     uint32
	XPelsPerMeter int32
	YPelsPerMeter int32
	ClrUsed       uint32
	ClrImportant  uint32
}

type bitmapInfo struct {
	Header bitmapInfoHeader
	Colors [1]uint32
}

type keyboardInput struct {
	VirtualKey uint16
	Scan       uint16
	Flags      uint32
	Time       uint32
	ExtraInfo  uintptr
}

type input struct {
	Type     uint32
	Pad      uint32
	Key      keyboardInput
	Reserved [8]byte // INPUT's union is sized for MOUSEINPUT on 64-bit Windows.
}

type guiThreadInfo struct {
	Size                                               uint32
	Flags                                              uint32
	Active, Focus, Capture, MenuOwner, MoveSize, Caret uintptr
	CaretRect                                          struct{ Left, Top, Right, Bottom int32 }
}

type windowsDriver struct{}

func NewDesktop() Desktop { return windowsDriver{} }

func (windowsDriver) Available() map[string]bool {
	uia := uiaAvailable()
	return map[string]bool{"screen.capture": true, "pointer.move": true, "pointer.click": true, "keyboard.type": true, "keyboard.key": true, "clipboard.read": true, "clipboard.write": true, "window.list": true, "browser.status": true, "window.focus": true,
		"ui.inspect": uia, "ui.focus": uia, "ui.invoke": uia,
		"browser.navigate": false, "browser.snapshot": false, "browser.query": false, "browser.click": false, "browser.fill": false, "browser.select": false, "browser.tabs": false, "browser.back": false, "browser.screenshot": false}
}

func (windowsDriver) Capture(ctx context.Context) (image.Image, Display, error) {
	if err := ctx.Err(); err != nil {
		return nil, Display{}, err
	}
	w, _, _ := getSystemMetrics.Call(0)
	h, _, _ := getSystemMetrics.Call(1)
	width, height := int(w), int(h)
	if width <= 0 || height <= 0 || width > 16384 || height > 16384 {
		return nil, Display{}, fmt.Errorf("display dimensions unavailable")
	}
	screen, _, _ := getDC.Call(0)
	if screen == 0 {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	defer releaseDC.Call(0, screen)
	mem, _, _ := createCompatibleDC.Call(screen)
	if mem == 0 {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	defer deleteDC.Call(mem)
	bmp, _, _ := createCompatibleBitmap.Call(screen, uintptr(width), uintptr(height))
	if bmp == 0 {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	defer deleteObject.Call(bmp)
	old, _, _ := selectObject.Call(mem, bmp)
	defer selectObject.Call(mem, old)
	ok, _, _ := bitBlt.Call(mem, 0, 0, uintptr(width), uintptr(height), screen, 0, 0, srccopy)
	if ok == 0 {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	info := bitmapInfo{Header: bitmapInfoHeader{Size: uint32(unsafe.Sizeof(bitmapInfoHeader{})), Width: int32(width), Height: -int32(height), Planes: 1, BitCount: 32, Compression: biRGB}}
	raw := make([]byte, width*height*4)
	n, _, _ := getDIBits.Call(mem, bmp, 0, uintptr(height), uintptr(unsafe.Pointer(&raw[0])), uintptr(unsafe.Pointer(&info)), 0)
	if n == 0 {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	img := image.NewRGBA(image.Rect(0, 0, width, height))
	for i := 0; i < len(raw); i += 4 {
		img.Pix[i], img.Pix[i+1], img.Pix[i+2], img.Pix[i+3] = raw[i+2], raw[i+1], raw[i], 0xff
	}
	return img, Display{Width: width, Height: height, Name: "primary"}, nil
}

func (windowsDriver) Move(ctx context.Context, x, y int) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	w, _, _ := getSystemMetrics.Call(0)
	h, _, _ := getSystemMetrics.Call(1)
	if x < 0 || y < 0 || x >= int(w) || y >= int(h) {
		return fmt.Errorf("coordinates outside primary display")
	}
	r, _, _ := setCursorPos.Call(uintptr(x), uintptr(y))
	if r == 0 {
		return fmt.Errorf("pointer move failed")
	}
	return nil
}

func (d windowsDriver) Click(ctx context.Context, x, y int, button string, count int) error {
	if count < 1 || count > 2 {
		return fmt.Errorf("click count must be one or two")
	}
	if err := d.Move(ctx, x, y); err != nil {
		return err
	}
	flags := map[string][2]uintptr{"left": {0x0002, 0x0004}, "right": {0x0008, 0x0010}, "middle": {0x0020, 0x0040}}
	pair, ok := flags[button]
	if !ok {
		return fmt.Errorf("unsupported mouse button")
	}
	for i := 0; i < count; i++ {
		if err := ctx.Err(); err != nil {
			return err
		}
		mouseEvent.Call(pair[0], 0, 0, 0, 0)
		mouseEvent.Call(pair[1], 0, 0, 0, 0)
		if i+1 < count {
			time.Sleep(80 * time.Millisecond)
		}
	}
	return nil
}

func sendUnicode(r rune) error {
	units := utf16.Encode([]rune{r})
	for _, unit := range units {
		pair := [2]input{{Type: 1, Key: keyboardInput{Scan: unit, Flags: keyUnicode}}, {Type: 1, Key: keyboardInput{Scan: unit, Flags: keyUnicode | keyUp}}}
		if n, _, _ := sendInput.Call(2, uintptr(unsafe.Pointer(&pair[0])), unsafe.Sizeof(pair[0])); n != 2 {
			return fmt.Errorf("keyboard input failed")
		}
	}
	return nil
}

func (windowsDriver) Type(ctx context.Context, text string) error {
	return fmt.Errorf("keyboard input requires worker focus authorization")
}

func resolveWindowThreadID(hwnd uintptr, call func(uintptr, *uint32) uintptr) (uintptr, error) {
	var processID uint32
	threadID := call(hwnd, &processID)
	if threadID == 0 {
		return 0, fmt.Errorf("foreground thread unavailable")
	}
	return threadID, nil
}

func focusedControl(ctx context.Context, expected uintptr) (uintptr, error) {
	if err := ctx.Err(); err != nil {
		return 0, err
	}
	top, _, _ := getForegroundWindow.Call()
	if top == 0 || top != expected || !(windowsDriver{}).ValidateWindow(ctx, fmt.Sprintf("0x%X", top)) {
		return 0, fmt.Errorf("authorized window is not current foreground")
	}
	threadID, err := resolveWindowThreadID(top, func(hwnd uintptr, processID *uint32) uintptr {
		r, _, _ := getWindowThreadProcessID.Call(hwnd, uintptr(unsafe.Pointer(processID)))
		return r
	})
	if err != nil {
		return 0, err
	}
	info := guiThreadInfo{Size: uint32(unsafe.Sizeof(guiThreadInfo{}))}
	if r, _, _ := getGUIThreadInfo.Call(threadID, uintptr(unsafe.Pointer(&info))); r == 0 || !focusBelongsTo(top, info.Active, info.Focus, func(parent, child uintptr) bool { result, _, _ := isChild.Call(parent, child); return result != 0 }) {
		return 0, fmt.Errorf("focused child is outside authorized window")
	}
	return info.Focus, nil
}

func focusBelongsTo(top, active, focus uintptr, childOf func(uintptr, uintptr) bool) bool {
	return top != 0 && active != 0 && focus != 0 && (active == top || childOf(top, active)) && (focus == top || childOf(top, focus))
}
func focusContinuous(expectedTop, expectedFocus, currentTop, currentFocus uintptr) bool {
	return expectedTop != 0 && expectedFocus != 0 && expectedTop == currentTop && expectedFocus == currentFocus
}

func waitForWindowActivation(ctx context.Context, id string, target uintptr, validate func() bool, activate func(uintptr) bool, foreground func() uintptr, pause func(time.Duration), timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		if !validate() {
			return fmt.Errorf("window became stale before activation")
		}
		accepted := activate(target)
		if accepted && foreground() == target {
			return nil
		}
		if !time.Now().Before(deadline) {
			return fmt.Errorf("window activation denied or foreground postcondition not met")
		}
		pause(25 * time.Millisecond)
	}
}

func authorizedHWND(id string) (uintptr, error) {
	if len(id) < 3 || len(id) > 256 || !strings.HasPrefix(strings.ToLower(id), "0x") {
		return 0, fmt.Errorf("invalid authorized window")
	}
	n, err := strconv.ParseUint(id[2:], 16, uintptrBits())
	if err != nil || n == 0 {
		return 0, fmt.Errorf("invalid authorized window")
	}
	return uintptr(n), nil
}

func (windowsDriver) TypeInWindow(ctx context.Context, id, text string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if text == "" || len(text) > 4096 {
		return fmt.Errorf("invalid keyboard text")
	}
	hwnd, err := authorizedHWND(id)
	if err != nil {
		return err
	}
	focus, err := focusedControl(ctx, hwnd)
	if err != nil {
		return err
	}
	sequence := make([]input, 0, len([]rune(text))*2)
	for _, r := range text {
		for _, unit := range utf16.Encode([]rune{r}) {
			sequence = append(sequence, input{Type: 1, Key: keyboardInput{Scan: unit, Flags: keyUnicode}}, input{Type: 1, Key: keyboardInput{Scan: unit, Flags: keyUnicode | keyUp}})
		}
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	activeFocus, err := focusedControl(ctx, hwnd)
	active, _, _ := getForegroundWindow.Call()
	if err != nil || !focusContinuous(hwnd, focus, active, activeFocus) {
		return fmt.Errorf("authorized focus changed before keyboard input")
	}
	if n, _, _ := sendInput.Call(uintptr(len(sequence)), uintptr(unsafe.Pointer(&sequence[0])), unsafe.Sizeof(sequence[0])); n != uintptr(len(sequence)) {
		return fmt.Errorf("keyboard input failed")
	}
	return nil
}

func (windowsDriver) Key(ctx context.Context, key string, modifiers []string) error {
	return fmt.Errorf("keyboard input requires worker focus authorization")
}

func (windowsDriver) KeyInWindow(ctx context.Context, id, key string, modifiers []string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if len(modifiers) != 0 {
		return fmt.Errorf("modifiers are not supported by the chat-orchestrator profile")
	}
	codes := map[string]uint16{"ENTER": 0x0d, "TAB": 0x09, "ESC": 0x1b, "SPACE": 0x20, "BACKSPACE": 0x08, "DELETE": 0x2e,
		"ARROW_LEFT": 0x25, "ARROW_UP": 0x26, "ARROW_RIGHT": 0x27, "ARROW_DOWN": 0x28, "HOME": 0x24, "END": 0x23}
	code, ok := codes[key]
	if !ok {
		return fmt.Errorf("unsupported chat-orchestrator key")
	}
	hwnd, err := authorizedHWND(id)
	if err != nil {
		return err
	}
	focus, err := focusedControl(ctx, hwnd)
	if err != nil {
		return err
	}
	pair := [2]input{{Type: 1, Key: keyboardInput{VirtualKey: code}}, {Type: 1, Key: keyboardInput{VirtualKey: code, Flags: keyUp}}}
	if err := ctx.Err(); err != nil {
		return err
	}
	activeFocus, err := focusedControl(ctx, hwnd)
	active, _, _ := getForegroundWindow.Call()
	if err != nil || !focusContinuous(hwnd, focus, active, activeFocus) {
		return fmt.Errorf("authorized focus changed before keyboard input")
	}
	if n, _, _ := sendInput.Call(2, uintptr(unsafe.Pointer(&pair[0])), unsafe.Sizeof(pair[0])); n != 2 {
		return fmt.Errorf("keyboard input failed")
	}
	return nil
}

func (windowsDriver) Windows(ctx context.Context) ([]Window, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	active, _, _ := getForegroundWindow.Call()
	var result []Window
	cb := syscall.NewCallback(func(hwnd uintptr, _ uintptr) uintptr {
		visible, _, _ := isWindowVisible.Call(hwnd)
		if visible == 0 {
			return 1
		}
		buf := make([]uint16, 512)
		n, _, _ := getWindowText.Call(hwnd, uintptr(unsafe.Pointer(&buf[0])), uintptr(len(buf)))
		title := strings.TrimSpace(syscall.UTF16ToString(buf[:n]))
		if title != "" {
			result = append(result, Window{ID: fmt.Sprintf("0x%X", hwnd), Title: title, Active: hwnd == active})
		}
		return 1
	})
	if r, _, _ := enumWindows.Call(cb, 0); r == 0 {
		return nil, fmt.Errorf("window enumeration failed")
	}
	return result, nil
}

func (windowsDriver) Focus(ctx context.Context, id string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if len(id) < 3 || len(id) > 256 || !(windowsDriver{}).ValidateWindow(ctx, id) {
		return fmt.Errorf("window is stale or not a current top-level window")
	}
	n, err := strconv.ParseUint(strings.TrimPrefix(strings.ToLower(id), "0x"), 16, uintptrBits())
	if err != nil || n == 0 {
		return fmt.Errorf("invalid window id")
	}
	showWindow.Call(uintptr(n), 9)
	return waitForWindowActivation(ctx, id, uintptr(n), func() bool { return (windowsDriver{}).ValidateWindow(ctx, id) }, func(target uintptr) bool { r, _, _ := setForegroundWindow.Call(target); return r != 0 }, func() uintptr { active, _, _ := getForegroundWindow.Call(); return active }, time.Sleep, 350*time.Millisecond)
}

func uintptrBits() int { return int(unsafe.Sizeof(uintptr(0)) * 8) }

func (windowsDriver) ClipboardRead(ctx context.Context) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	if r, _, _ := openClipboard.Call(0); r == 0 {
		return "", fmt.Errorf("clipboard unavailable")
	}
	defer closeClipboard.Call()
	h, _, _ := getClipboardData.Call(cfUnicodeText)
	if h == 0 {
		return "", fmt.Errorf("clipboard has no text")
	}
	p, _, _ := globalLock.Call(h)
	if p == 0 {
		return "", fmt.Errorf("clipboard unavailable")
	}
	defer globalUnlock.Call(h)
	size, _, _ := globalSize.Call(h)
	if size < 2 || size > 2*(1<<20) || size%2 != 0 {
		return "", fmt.Errorf("clipboard text exceeds limit")
	}
	raw := make([]byte, int(size))
	var bytesRead uintptr
	process, _, _ := getCurrentProcess.Call()
	ok, _, _ := readProcessMemory.Call(process, p, uintptr(unsafe.Pointer(&raw[0])), size, uintptr(unsafe.Pointer(&bytesRead)))
	runtime.KeepAlive(raw)
	if ok == 0 || bytesRead < 2 {
		return "", fmt.Errorf("clipboard unavailable")
	}
	units := make([]uint16, int(bytesRead/2))
	for i := range units {
		units[i] = binary.LittleEndian.Uint16(raw[i*2 : i*2+2])
	}
	n := 0
	for n < len(units) && units[n] != 0 {
		n++
	}
	if n == len(units) {
		return "", fmt.Errorf("clipboard text exceeds limit")
	}
	return syscall.UTF16ToString(units[:n]), nil
}

func (windowsDriver) ClipboardWrite(ctx context.Context, text string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	if len(text) > 1<<20 {
		return fmt.Errorf("clipboard text exceeds limit")
	}
	wide := utf16.Encode([]rune(text))
	wide = append(wide, 0)
	size := uintptr(len(wide) * 2)
	h, _, _ := globalAlloc.Call(gmemMoveable, size)
	if h == 0 {
		return fmt.Errorf("clipboard unavailable")
	}
	p, _, _ := globalLock.Call(h)
	if p == 0 {
		globalFree.Call(h)
		return fmt.Errorf("clipboard unavailable")
	}
	raw := make([]byte, int(size))
	for i, unit := range wide {
		binary.LittleEndian.PutUint16(raw[i*2:i*2+2], unit)
	}
	var bytesWritten uintptr
	process, _, _ := getCurrentProcess.Call()
	ok, _, _ := writeProcessMemory.Call(process, p, uintptr(unsafe.Pointer(&raw[0])), size, uintptr(unsafe.Pointer(&bytesWritten)))
	runtime.KeepAlive(raw)
	if ok == 0 || bytesWritten != size {
		globalUnlock.Call(h)
		globalFree.Call(h)
		return fmt.Errorf("clipboard unavailable")
	}
	globalUnlock.Call(h)
	if r, _, _ := openClipboard.Call(0); r == 0 {
		globalFree.Call(h)
		return fmt.Errorf("clipboard unavailable")
	}
	defer closeClipboard.Call()
	if r, _, _ := emptyClipboard.Call(); r == 0 {
		globalFree.Call(h)
		return fmt.Errorf("clipboard unavailable")
	}
	if r, _, _ := setClipboardData.Call(cfUnicodeText, h); r == 0 {
		globalFree.Call(h)
		return fmt.Errorf("clipboard unavailable")
	}
	return nil
}

func (windowsDriver) OpenURL(ctx context.Context, raw string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	action, _ := syscall.UTF16PtrFromString("open")
	target, err := syscall.UTF16PtrFromString(raw)
	if err != nil {
		return fmt.Errorf("invalid URL")
	}
	r, _, _ := shellExecute.Call(0, uintptr(unsafe.Pointer(action)), uintptr(unsafe.Pointer(target)), 0, 0, 1)
	if r <= 32 {
		return fmt.Errorf("browser launch failed")
	}
	return nil
}
