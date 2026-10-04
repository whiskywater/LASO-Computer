//go:build linux

package platform

import (
	"context"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"time"

	"github.com/jezek/xgb"
	"github.com/jezek/xgb/xproto"
	"github.com/jezek/xgb/xtest"
)

type linuxDriver struct{}

func NewDesktop() Desktop { return linuxDriver{} }

func (linuxDriver) Available() map[string]bool {
	result := map[string]bool{"screen.capture": false, "pointer.move": false, "pointer.click": false, "keyboard.type": false, "keyboard.key": false, "clipboard.read": false, "clipboard.write": false, "window.list": false, "window.focus": false, "browser.navigate": false}
	conn, err := xgb.NewConn()
	if err == nil {
		screen, err := rootScreen(conn)
		if err == nil && screen.WidthInPixels > 0 && screen.HeightInPixels > 0 {
			result["screen.capture"] = true
			result["window.list"], result["window.focus"] = true, true
			if xtest.Init(conn) == nil {
				result["pointer.move"], result["pointer.click"], result["keyboard.type"], result["keyboard.key"] = true, true, true, true
			}
		}
		conn.Close()
	}
	if _, err := exec.LookPath("xclip"); err == nil && os.Getenv("DISPLAY") != "" {
		result["clipboard.read"], result["clipboard.write"] = true, true
	}
	if _, err := exec.LookPath("xdg-open"); err == nil {
		result["browser.navigate"] = true
	}
	return result
}

func connect() (*xgb.Conn, *xproto.ScreenInfo, error) {
	if os.Getenv("DISPLAY") == "" {
		return nil, nil, fmt.Errorf("graphical display unavailable")
	}
	c, err := xgb.NewConn()
	if err != nil {
		return nil, nil, fmt.Errorf("graphical display unavailable")
	}
	screen, err := rootScreen(c)
	if err != nil {
		c.Close()
		return nil, nil, err
	}
	return c, screen, nil
}

func rootScreen(c *xgb.Conn) (*xproto.ScreenInfo, error) {
	setup := xproto.Setup(c)
	if len(setup.Roots) == 0 || int(c.DefaultScreen) >= len(setup.Roots) {
		return nil, fmt.Errorf("graphical display unavailable")
	}
	return setup.DefaultScreen(c), nil
}

func (linuxDriver) Capture(ctx context.Context) (image.Image, Display, error) {
	if err := ctx.Err(); err != nil {
		return nil, Display{}, err
	}
	c, screen, err := connect()
	if err != nil {
		return nil, Display{}, err
	}
	defer c.Close()
	w, h := int(screen.WidthInPixels), int(screen.HeightInPixels)
	if w < 1 || h < 1 || w > 16384 || h > 16384 {
		return nil, Display{}, fmt.Errorf("display dimensions unavailable")
	}
	reply, err := xproto.GetImage(c, xproto.ImageFormatZPixmap, xproto.Drawable(screen.Root), 0, 0, uint16(w), uint16(h), 0xffffffff).Reply()
	if err != nil || reply == nil {
		return nil, Display{}, fmt.Errorf("screen capture unavailable")
	}
	setup := xproto.Setup(c)
	var bpp, scanlinePad uint8
	for _, format := range setup.PixmapFormats {
		if format.Depth == screen.RootDepth {
			bpp, scanlinePad = format.BitsPerPixel, format.ScanlinePad
			break
		}
	}
	if bpp != 24 && bpp != 32 || scanlinePad == 0 {
		return nil, Display{}, fmt.Errorf("unsupported display pixel format")
	}
	var visual *xproto.VisualInfo
	for _, depth := range screen.AllowedDepths {
		if depth.Depth == screen.RootDepth {
			for i := range depth.Visuals {
				if depth.Visuals[i].VisualId == screen.RootVisual {
					visual = &depth.Visuals[i]
					break
				}
			}
		}
	}
	if visual == nil {
		return nil, Display{}, fmt.Errorf("unsupported display visual")
	}
	bytesPerPixel := int(bpp / 8)
	rowBytes := ((w*int(bpp) + int(scanlinePad) - 1) / int(scanlinePad)) * (int(scanlinePad) / 8)
	if rowBytes <= 0 || len(reply.Data) < rowBytes*h {
		return nil, Display{}, fmt.Errorf("invalid screen capture data")
	}
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		if y&31 == 0 {
			if err := ctx.Err(); err != nil {
				return nil, Display{}, err
			}
		}
		for x := 0; x < w; x++ {
			off := y*rowBytes + x*bytesPerPixel
			if off+bytesPerPixel > len(reply.Data) {
				break
			}
			var px uint32
			if setup.ImageByteOrder == 0 {
				for i := 0; i < bytesPerPixel; i++ {
					px |= uint32(reply.Data[off+i]) << uint(8*i)
				}
			} else {
				for i := 0; i < bytesPerPixel; i++ {
					px = (px << 8) | uint32(reply.Data[off+i])
				}
			}
			r := maskChannel(px, visual.RedMask)
			g := maskChannel(px, visual.GreenMask)
			b := maskChannel(px, visual.BlueMask)
			img.SetRGBA(x, y, color.RGBA{R: r, G: g, B: b, A: 0xff})
		}
	}
	return img, Display{Width: w, Height: h, Name: "primary"}, nil
}

func maskChannel(pixel, mask uint32) uint8 {
	if mask == 0 {
		return 0
	}
	shift := uint(0)
	for (mask>>shift)&1 == 0 {
		shift++
	}
	value := (pixel & mask) >> shift
	max := mask >> shift
	if max == 0 {
		return 0
	}
	return uint8(uint64(value) * 255 / uint64(max))
}

func (linuxDriver) Move(ctx context.Context, x, y int) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	c, s, err := connect()
	if err != nil {
		return err
	}
	defer c.Close()
	if err := xtest.Init(c); err != nil {
		return fmt.Errorf("XTEST input unavailable")
	}
	if x < 0 || y < 0 || x >= int(s.WidthInPixels) || y >= int(s.HeightInPixels) {
		return fmt.Errorf("coordinates outside primary display")
	}
	if err := xtest.FakeInputChecked(c, 6, 0, 0, s.Root, int16(x), int16(y), 0).Check(); err != nil {
		return fmt.Errorf("pointer move failed")
	}
	return nil
}

func (linuxDriver) Click(ctx context.Context, x, y int, button string, count int) error {
	if count < 1 || count > 2 {
		return fmt.Errorf("click count must be one or two")
	}
	if err := (linuxDriver{}).Move(ctx, x, y); err != nil {
		return err
	}
	c, s, err := connect()
	if err != nil {
		return err
	}
	defer c.Close()
	if err := xtest.Init(c); err != nil {
		return fmt.Errorf("XTEST input unavailable")
	}
	btn := map[string]byte{"left": 1, "middle": 2, "right": 3}[button]
	if btn == 0 {
		return fmt.Errorf("unsupported mouse button")
	}
	for i := 0; i < count; i++ {
		if err := ctx.Err(); err != nil {
			return err
		}
		if err := xtest.FakeInputChecked(c, 4, btn, 0, s.Root, int16(x), int16(y), 0).Check(); err != nil {
			return fmt.Errorf("pointer click failed")
		}
		if err := xtest.FakeInputChecked(c, 5, btn, 0, s.Root, int16(x), int16(y), 0).Check(); err != nil {
			return fmt.Errorf("pointer click failed")
		}
		if i+1 < count {
			time.Sleep(80 * time.Millisecond)
		}
	}
	return nil
}

func keycode(c *xgb.Conn, sym xproto.Keysym) (xproto.Keycode, bool, bool) {
	setup := xproto.Setup(c)
	if setup.MaxKeycode < setup.MinKeycode {
		return 0, false, false
	}
	count := byte(setup.MaxKeycode - setup.MinKeycode + 1)
	reply, err := xproto.GetKeyboardMapping(c, setup.MinKeycode, count).Reply()
	if err != nil || reply == nil {
		return 0, false, false
	}
	per := int(reply.KeysymsPerKeycode)
	if per == 0 {
		return 0, false, false
	}
	for i := 0; i < int(count); i++ {
		for j := 0; j < per; j++ {
			if reply.Keysyms[i*per+j] == sym {
				return setup.MinKeycode + xproto.Keycode(i), true, j%2 == 1
			}
		}
	}
	return 0, false, false
}

func fakeKey(c *xgb.Conn, root xproto.Window, code xproto.Keycode, down bool) error {
	event := byte(xproto.KeyRelease)
	if down {
		event = byte(xproto.KeyPress)
	}
	return xtest.FakeInputChecked(c, event, byte(code), 0, root, 0, 0, 0).Check()
}

func (linuxDriver) Type(ctx context.Context, text string) error {
	c, s, err := connect()
	if err != nil {
		return err
	}
	defer c.Close()
	if err := xtest.Init(c); err != nil {
		return fmt.Errorf("XTEST input unavailable")
	}
	for _, r := range text {
		if err := ctx.Err(); err != nil {
			return err
		}
		if r > 127 {
			return fmt.Errorf("this X11 input adapter currently supports ASCII text only")
		}
		sym := xproto.Keysym(r)
		if r == '\n' {
			sym = 0xff0d
		}
		if r == '\t' {
			sym = 0xff09
		}
		code, ok, shift := keycode(c, sym)
		if !ok {
			return fmt.Errorf("text key is unavailable on this keyboard layout")
		}
		if shift {
			shiftCode, found, _ := keycode(c, 0xffe1)
			if !found {
				return fmt.Errorf("shift key unavailable")
			}
			if err := fakeKey(c, s.Root, shiftCode, true); err != nil {
				return fmt.Errorf("keyboard input failed")
			}
		}
		if err := fakeKey(c, s.Root, code, true); err != nil {
			return fmt.Errorf("keyboard input failed")
		}
		if err := fakeKey(c, s.Root, code, false); err != nil {
			return fmt.Errorf("keyboard input failed")
		}
		if shift {
			shiftCode, _, _ := keycode(c, 0xffe1)
			if err := fakeKey(c, s.Root, shiftCode, false); err != nil {
				return fmt.Errorf("keyboard input failed")
			}
		}
	}
	return nil
}

func (linuxDriver) Key(ctx context.Context, key string, modifiers []string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	symbols := map[string]xproto.Keysym{"enter": 0xff0d, "tab": 0xff09, "escape": 0xff1b, "space": 0x20, "backspace": 0xff08, "delete": 0xffff, "left": 0xff51, "up": 0xff52, "right": 0xff53, "down": 0xff54, "home": 0xff50, "end": 0xff57, "pageup": 0xff55, "pagedown": 0xff56}
	key = strings.ToLower(key)
	if len(key) == 1 {
		symbols[key] = xproto.Keysym(key[0])
	}
	if len(key) == 2 && key[0] == 'f' {
		if n, e := strconv.Atoi(key[1:]); e == nil && n >= 1 && n <= 12 {
			symbols[key] = xproto.Keysym(0xffbd + n)
		}
	}
	sym, ok := symbols[key]
	if !ok {
		return fmt.Errorf("unsupported key")
	}
	mods := map[string]xproto.Keysym{"shift": 0xffe1, "ctrl": 0xffe3, "control": 0xffe3, "alt": 0xffe9}
	c, s, err := connect()
	if err != nil {
		return err
	}
	defer c.Close()
	if err := xtest.Init(c); err != nil {
		return fmt.Errorf("XTEST input unavailable")
	}
	var codes []xproto.Keycode
	for _, name := range modifiers {
		v, ok := mods[strings.ToLower(name)]
		if !ok {
			return fmt.Errorf("unsupported modifier")
		}
		code, found, _ := keycode(c, v)
		if !found {
			return fmt.Errorf("modifier unavailable")
		}
		codes = append(codes, code)
	}
	for _, code := range codes {
		if err := fakeKey(c, s.Root, code, true); err != nil {
			return fmt.Errorf("keyboard input failed")
		}
	}
	target, found, _ := keycode(c, sym)
	if !found {
		return fmt.Errorf("key unavailable on this keyboard layout")
	}
	if err := fakeKey(c, s.Root, target, true); err != nil {
		return fmt.Errorf("keyboard input failed")
	}
	if err := fakeKey(c, s.Root, target, false); err != nil {
		return fmt.Errorf("keyboard input failed")
	}
	for i := len(codes) - 1; i >= 0; i-- {
		if err := fakeKey(c, s.Root, codes[i], false); err != nil {
			return fmt.Errorf("keyboard input failed")
		}
	}
	return nil
}

func intern(c *xgb.Conn, name string) (xproto.Atom, error) {
	r, e := xproto.InternAtom(c, false, uint16(len(name)), name).Reply()
	if e != nil || r == nil {
		return 0, fmt.Errorf("window metadata unavailable")
	}
	return r.Atom, nil
}

func property(c *xgb.Conn, w xproto.Window, a xproto.Atom, limit uint32) (*xproto.GetPropertyReply, error) {
	r, e := xproto.GetProperty(c, false, w, a, xproto.AtomAny, 0, limit).Reply()
	if e != nil || r == nil {
		return nil, fmt.Errorf("window metadata unavailable")
	}
	return r, nil
}

func readTitle(c *xgb.Conn, w xproto.Window) (string, error) {
	name, err := intern(c, "_NET_WM_NAME")
	if err == nil {
		p, e := property(c, w, name, 256)
		if e == nil && len(p.Value) > 0 {
			return truncateTitle(string(p.Value)), nil
		}
	}
	name, err = intern(c, "WM_NAME")
	if err != nil {
		return "", err
	}
	p, err := property(c, w, name, 256)
	if err != nil {
		return "", err
	}
	return truncateTitle(string(p.Value)), nil
}

func truncateTitle(s string) string {
	s = strings.TrimSpace(strings.ToValidUTF8(s, "�"))
	if len(s) > 512 {
		s = s[:512]
	}
	return s
}

func (linuxDriver) Windows(ctx context.Context) ([]Window, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	c, s, err := connect()
	if err != nil {
		return nil, err
	}
	defer c.Close()
	listAtom, err := intern(c, "_NET_CLIENT_LIST")
	if err != nil {
		return nil, err
	}
	activeAtom, err := intern(c, "_NET_ACTIVE_WINDOW")
	if err != nil {
		return nil, err
	}
	list, err := property(c, s.Root, listAtom, 8192)
	if err != nil {
		return nil, err
	}
	active, err := property(c, s.Root, activeAtom, 1)
	if err != nil {
		return nil, err
	}
	activeID := uint32(0)
	if len(active.Value) >= 4 {
		activeID = xgb.Get32(active.Value)
	}
	result := make([]Window, 0, list.ValueLen)
	for i := 0; i+4 <= len(list.Value); i += 4 {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		id := xproto.Window(xgb.Get32(list.Value[i:]))
		title, e := readTitle(c, id)
		if e == nil && title != "" {
			result = append(result, Window{ID: fmt.Sprintf("0x%X", uint32(id)), Title: title, Active: uint32(id) == activeID})
		}
	}
	return result, nil
}

func (linuxDriver) Focus(ctx context.Context, id string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	raw := strings.TrimPrefix(strings.ToLower(id), "0x")
	n, e := strconv.ParseUint(raw, 16, 32)
	if e != nil || n == 0 {
		return fmt.Errorf("invalid window id")
	}
	c, s, err := connect()
	if err != nil {
		return err
	}
	defer c.Close()
	atom, err := intern(c, "_NET_ACTIVE_WINDOW")
	if err != nil {
		return err
	}
	data := xproto.ClientMessageDataUnionData32New([]uint32{2, 0, 0, 0, 0})
	event := xproto.ClientMessageEvent{Format: 32, Window: xproto.Window(n), Type: atom, Data: data}
	if err := xproto.SendEventChecked(c, false, s.Root, xproto.EventMaskSubstructureNotify|xproto.EventMaskSubstructureRedirect, string(event.Bytes())).Check(); err != nil {
		return fmt.Errorf("window focus failed")
	}
	return nil
}

func (linuxDriver) ClipboardRead(ctx context.Context) (string, error) {
	cmd := exec.CommandContext(ctx, "xclip", "-selection", "clipboard", "-o", "-target", "UTF8_STRING")
	var b limitedBuffer
	b.limit = 1 << 20
	cmd.Stdout = &b
	if err := cmd.Run(); err != nil {
		return "", fmt.Errorf("clipboard unavailable")
	}
	return b.String(), nil
}
func (linuxDriver) ClipboardWrite(ctx context.Context, text string) error {
	if len(text) > 1<<20 {
		return fmt.Errorf("clipboard text exceeds limit")
	}
	cmd := exec.CommandContext(ctx, "xclip", "-selection", "clipboard", "-i", "-target", "UTF8_STRING")
	cmd.Stdin = strings.NewReader(text)
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("clipboard unavailable")
	}
	return nil
}

type limitedBuffer struct {
	b         []byte
	limit     int
	truncated bool
}

func (w *limitedBuffer) Write(p []byte) (int, error) {
	n := len(p)
	room := w.limit - len(w.b)
	if room > 0 {
		if room > len(p) {
			room = len(p)
		}
		w.b = append(w.b, p[:room]...)
	}
	if room < n {
		w.truncated = true
	}
	return n, nil
}
func (w *limitedBuffer) String() string { return string(w.b) }

var _ io.Writer = (*limitedBuffer)(nil)

func (linuxDriver) OpenURL(ctx context.Context, raw string) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	cmd := exec.Command("xdg-open", raw)
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("browser launch failed")
	}
	_ = cmd.Process.Release()
	return nil
}
