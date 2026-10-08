//go:build windows

package platform

import (
	"context"
	"fmt"
	"runtime"
	"strings"
	"syscall"
	"unicode/utf8"
	"unsafe"
)

var (
	ole32UIA                    = syscall.NewLazyDLL("ole32.dll")
	oleaut32UIA                 = syscall.NewLazyDLL("oleaut32.dll")
	coInitExUIA                 = ole32UIA.NewProc("CoInitializeEx")
	coUninitUIA                 = ole32UIA.NewProc("CoUninitialize")
	coCreateUIA                 = ole32UIA.NewProc("CoCreateInstance")
	variantClear                = oleaut32UIA.NewProc("VariantClear")
	sysStringLen                = oleaut32UIA.NewProc("SysStringLen")
	safeArrayGetLB              = oleaut32UIA.NewProc("SafeArrayGetLBound")
	safeArrayGetUB              = oleaut32UIA.NewProc("SafeArrayGetUBound")
	safeArrayGetElement         = oleaut32UIA.NewProc("SafeArrayGetElement")
	getForegroundWindowUIA      = user32.NewProc("GetForegroundWindow")
	getWindowThreadProcessIDUIA = user32.NewProc("GetWindowThreadProcessId")
	isWindowUIA                 = user32.NewProc("IsWindow")
	getWindowRectUIA            = user32.NewProc("GetWindowRect")
	getCursorPosUIA             = user32.NewProc("GetCursorPos")
	openProcessUIA              = kernel32.NewProc("OpenProcess")
	queryProcessImageUIA        = kernel32.NewProc("QueryFullProcessImageNameW")
	closeHandleUIA              = kernel32.NewProc("CloseHandle")
)

type uiaGUID struct {
	Data1        uint32
	Data2, Data3 uint16
	Data4        [8]byte
}

var (
	clsidUIAutomation            = uiaGUID{0xff48dba4, 0x60ef, 0x4201, [8]byte{0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}}
	iidUIAutomation              = uiaGUID{0x30cbe57d, 0xd9d0, 0x452a, [8]byte{0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}}
	iidUIAutomationInvokePattern = uiaGUID{0xfb377fbe, 0x8ea6, 0x46d5, [8]byte{0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}}
)

type uiaVariant struct {
	Vt, R1, R2, R3 uint16
	Value          [8]byte
}
type uiaRect struct{ Left, Top, Right, Bottom int32 }
type uiaPoint struct{ X, Y int32 }
type uiaDepthNode[T any] struct {
	value T
	depth int
}

const (
	vtBSTR                            = 8
	vtI4                              = 3
	vtBool                            = 11
	vtArray                           = 0x2000
	vtR8                              = 5
	uiaName                           = 30005
	uiaAutomation                     = 30011
	uiaControl                        = 30003
	uiaEnabled                        = 30010
	uiaFocusable                      = 30009
	uiaHasKeyboardFocus               = 30008
	uiaOffscreen                      = 30022
	uiaValue                          = 30045
	uiaBounds                         = 30001
	uiaInvoke                         = 10000
	uiaElementGetCurrentPatternAsSlot = 14
	metricVirtualLeft                 = 76
	metricVirtualTop                  = 77
	metricVirtualWidth                = 78
	metricVirtualHeight               = 79
)

var controlTypes = map[string]int32{
	"button": 50000, "calendar": 50001, "checkbox": 50002, "combobox": 50003, "edit": 50004, "hyperlink": 50005, "image": 50006, "listitem": 50007,
	"list": 50008, "menu": 50009, "menubar": 50010, "menuitem": 50011, "progressbar": 50012, "radiobutton": 50013, "scrollbar": 50014, "slider": 50015,
	"spinner": 50016, "statusbar": 50017, "tab": 50018, "tabitem": 50019, "text": 50020, "toolbar": 50021, "tooltip": 50022, "tree": 50023,
	"treeitem": 50024, "custom": 50025, "group": 50026, "thumb": 50027, "datagrid": 50028, "dataitem": 50029, "document": 50030,
	"splitbutton": 50031, "window": 50032, "pane": 50033, "header": 50034, "headeritem": 50035, "table": 50036, "titlebar": 50037, "separator": 50038,
}

func ptrArg(p unsafe.Pointer) uintptr { return uintptr(p) }

type uiaOwnedRefs struct {
	refs    []unsafe.Pointer
	release func(unsafe.Pointer)
	closed  bool
}

func newUIAOwnedRefs() *uiaOwnedRefs { return &uiaOwnedRefs{release: releaseCOM} }
func (r *uiaOwnedRefs) own(p unsafe.Pointer) unsafe.Pointer {
	if p != nil && !r.closed {
		r.refs = append(r.refs, p)
	}
	return p
}
func (r *uiaOwnedRefs) close() {
	if r == nil || r.closed {
		return
	}
	r.closed = true
	for i := len(r.refs) - 1; i >= 0; i-- {
		if p := r.refs[i]; p != nil {
			r.release(p)
		}
	}
	r.refs = nil
}

func comCall(object unsafe.Pointer, slot uintptr, args ...uintptr) (uintptr, error) {
	if object == nil {
		return 0, fmt.Errorf("UI Automation object unavailable")
	}
	vtable := *(*unsafe.Pointer)(object)
	fn := *(*uintptr)(unsafe.Add(vtable, slot*unsafe.Sizeof(uintptr(0))))
	callArgs := make([]uintptr, 1, len(args)+1)
	callArgs[0] = uintptr(object)
	callArgs = append(callArgs, args...)
	hr, _, _ := syscall.SyscallN(fn, callArgs...)
	if int32(hr) < 0 {
		return 0, fmt.Errorf("UI Automation call failed")
	}
	return hr, nil
}

type uiaCOMCaller func(unsafe.Pointer, uintptr, ...uintptr) (uintptr, error)

func getCurrentPatternAs(call uiaCOMCaller, element unsafe.Pointer, patternID uintptr, iid *uiaGUID) (unsafe.Pointer, error) {
	var pattern unsafe.Pointer
	if iid == nil {
		return nil, fmt.Errorf("UI Automation pattern IID unavailable")
	}
	if _, err := call(element, uiaElementGetCurrentPatternAsSlot, patternID, uintptr(unsafe.Pointer(iid)), uintptr(unsafe.Pointer(&pattern))); err != nil {
		return nil, err
	}
	if pattern == nil {
		return nil, fmt.Errorf("UI Automation pattern unavailable")
	}
	return pattern, nil
}

func releaseCOM(object unsafe.Pointer) {
	if object != nil {
		_, _ = comCall(object, 2)
	}
}
func startUIA() (unsafe.Pointer, error) {
	hr, _, _ := coInitExUIA.Call(0, 2)
	if int32(hr) < 0 {
		return nil, fmt.Errorf("UI Automation apartment unavailable")
	}
	var automation unsafe.Pointer
	hr, _, _ = coCreateUIA.Call(uintptr(unsafe.Pointer(&clsidUIAutomation)), 0, 1, uintptr(unsafe.Pointer(&iidUIAutomation)), uintptr(unsafe.Pointer(&automation)))
	if int32(hr) < 0 || automation == nil {
		if automation != nil {
			releaseCOM(automation)
		}
		coUninitUIA.Call()
		return nil, fmt.Errorf("UI Automation provider unavailable")
	}
	return automation, nil
}
func uiaAvailable() bool {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	a, e := startUIA()
	if e != nil {
		return false
	}
	defer coUninitUIA.Call()
	refs := newUIAOwnedRefs()
	defer refs.close()
	refs.own(a)
	var root unsafe.Pointer
	_, e = comCall(a, 5, uintptr(unsafe.Pointer(&root)))
	refs.own(root)
	return e == nil && root != nil
}

func (windowsDriver) ValidateWindow(ctx context.Context, id string) bool {
	hwnd, e := resolveUIAWindow(id)
	if e != nil || ctx.Err() != nil {
		return false
	}
	wins, e := (windowsDriver{}).Windows(ctx)
	if e != nil {
		return false
	}
	for _, w := range wins {
		if strings.EqualFold(w.ID, fmt.Sprintf("0x%X", hwnd)) {
			return true
		}
	}
	return false
}
func resolveUIAWindow(id string) (uintptr, error) {
	if id == "" {
		h, _, _ := getForegroundWindowUIA.Call()
		if h == 0 {
			return 0, fmt.Errorf("foreground window unavailable")
		}
		return h, nil
	}
	if len(id) < 3 || len(id) > 256 || !strings.HasPrefix(strings.ToLower(id), "0x") {
		return 0, fmt.Errorf("invalid window id")
	}
	var n uintptr
	for _, r := range id[2:] {
		n <<= 4
		switch {
		case r >= '0' && r <= '9':
			n |= uintptr(r - '0')
		case r >= 'a' && r <= 'f':
			n |= uintptr(r - 'a' + 10)
		case r >= 'A' && r <= 'F':
			n |= uintptr(r - 'A' + 10)
		default:
			return 0, fmt.Errorf("invalid window id")
		}
	}
	if n == 0 {
		return 0, fmt.Errorf("invalid window id")
	}
	ok, _, _ := isWindowUIA.Call(n)
	if ok == 0 {
		return 0, fmt.Errorf("window not found")
	}
	return n, nil
}

func boundedUIATraversal[T any](ctx context.Context, root T, maxDepth, maxMatches int,
	children func(T) ([]T, error), match func(T) bool, use func([]uiaDepthNode[T], bool) error) error {
	stack := []uiaDepthNode[T]{{value: root}}
	matches := make([]uiaDepthNode[T], 0)
	visited := 0
	truncated := false
	for len(stack) > 0 {
		if err := ctx.Err(); err != nil {
			return err
		}
		node := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		visited++
		if visited > 4096 {
			truncated = true
			break
		}
		var descendants []T
		if node.depth <= maxDepth {
			var err error
			descendants, err = children(node.value)
			if err != nil {
				return err
			}
		}
		if match(node.value) {
			matches = append(matches, node)
			if maxMatches > 0 && len(matches) >= maxMatches {
				truncated = len(stack) > 0 || len(descendants) > 0
				break
			}
		}
		if node.depth == maxDepth {
			if len(descendants) > 0 {
				truncated = true
			}
			continue
		}
		for i := len(descendants) - 1; i >= 0; i-- {
			stack = append(stack, uiaDepthNode[T]{value: descendants[i], depth: node.depth + 1})
		}
	}
	if truncated && maxMatches == 0 {
		return fmt.Errorf("UI Automation traversal bound reached")
	}
	return use(matches, truncated)
}

func searchUIA(ctx context.Context, hwnd uintptr, maxDepth, maxMatches int, match func(unsafe.Pointer) bool, use func([]uiaDepthNode[unsafe.Pointer], bool) error) error {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	a, e := startUIA()
	if e != nil {
		return e
	}
	defer coUninitUIA.Call()
	refs := newUIAOwnedRefs()
	defer refs.close()
	refs.own(a)
	var root unsafe.Pointer
	_, e = comCall(a, 6, hwnd, uintptr(unsafe.Pointer(&root)))
	refs.own(root)
	if e != nil || root == nil {
		return fmt.Errorf("UI Automation root unavailable")
	}
	var walker unsafe.Pointer
	_, e = comCall(a, 14, uintptr(unsafe.Pointer(&walker)))
	refs.own(walker)
	if e != nil || walker == nil {
		return fmt.Errorf("UI Automation control view unavailable")
	}
	children := func(parent unsafe.Pointer) ([]unsafe.Pointer, error) {
		var child unsafe.Pointer
		_, firstErr := comCall(walker, 5, ptrArg(parent), uintptr(unsafe.Pointer(&child)))
		refs.own(child)
		if firstErr != nil {
			return nil, nil
		}
		result := make([]unsafe.Pointer, 0)
		for child != nil {
			result = append(result, child)
			var next unsafe.Pointer
			_, nextErr := comCall(walker, 7, ptrArg(child), uintptr(unsafe.Pointer(&next)))
			refs.own(next)
			if nextErr != nil {
				break
			}
			child = next
		}
		return result, nil
	}
	return boundedUIATraversal(ctx, root, maxDepth, maxMatches, children, match, use)
}

func (windowsDriver) Inspect(ctx context.Context, id string, maxDepth, maxNodes int) ([]UIElement, bool, error) {
	if maxDepth < 0 || maxDepth > 16 || maxNodes < 1 || maxNodes > 256 {
		return nil, false, fmt.Errorf("invalid UI Automation bounds")
	}
	hwnd, e := resolveUIAWindow(id)
	if e != nil {
		return nil, false, e
	}
	if !(windowsDriver{}).ValidateWindow(ctx, id) {
		return nil, false, fmt.Errorf("window is stale or not a current top-level window")
	}
	out := []UIElement{}
	truncated := false
	e = searchUIA(ctx, hwnd, maxDepth, maxNodes, func(el unsafe.Pointer) bool {
		return !readUIABool(el, uiaOffscreen) && (readUIAString(el, uiaName) != "" || readUIAString(el, uiaAutomation) != "")
	}, func(found []uiaDepthNode[unsafe.Pointer], cut bool) error {
		for _, node := range found {
			el := node.value
			name, nc := boundedUIAString(readUIAString(el, uiaName), 256)
			id, ic := boundedUIAString(readUIAString(el, uiaAutomation), 128)
			if name == "" && id == "" {
				continue
			}
			out = append(out, UIElement{Name: name, NameTruncated: nc, AutomationID: id, AutomationIDTruncated: ic, ControlType: readUIAInt(el, uiaControl), Enabled: readUIABool(el, uiaEnabled), Depth: node.depth})
		}
		truncated = cut
		return nil
	})
	return out, truncated, e
}
func boundedUIAString(s string, max int) (string, bool) {
	if len(s) <= max {
		return s, false
	}
	cut := max
	for cut > 0 && !utf8.ValidString(s[:cut]) {
		cut--
	}
	return s[:cut], true
}

func (windowsDriver) FocusElement(ctx context.Context, id, target string) error {
	return actOnUIA(ctx, id, target, "focus")
}
func (windowsDriver) InvokeElement(ctx context.Context, id, target, action string) error {
	return actOnUIA(ctx, id, target, action)
}
func actOnUIA(ctx context.Context, id, target, action string) error {
	hwnd, e := resolveUIAWindow(id)
	if e != nil {
		return e
	}
	if !(windowsDriver{}).ValidateWindow(ctx, id) {
		return fmt.Errorf("window is stale or not a current top-level window")
	}
	if target == "" || len(target) > 512 {
		return fmt.Errorf("invalid UI Automation target")
	}
	applied := false
	for _, byName := range []bool{false, true} {
		rawFound := false
		e = searchUIA(ctx, hwnd, 16, 0, func(el unsafe.Pointer) bool {
			prop := int32(uiaAutomation)
			if byName {
				prop = uiaName
			}
			return readUIAString(el, prop) == target
		}, func(found []uiaDepthNode[unsafe.Pointer], cut bool) error {
			if cut {
				return fmt.Errorf("UI Automation target search was truncated")
			}
			rawFound = len(found) > 0
			if len(found) > 1 {
				return fmt.Errorf("UI Automation target is ambiguous")
			}
			actionable := []unsafe.Pointer{}
			for _, node := range found {
				el := node.value
				if !readUIABool(el, uiaEnabled) || readUIABool(el, uiaOffscreen) {
					continue
				}
				if action == "focus" && !readUIABool(el, uiaFocusable) {
					continue
				}
				if action == "click" || action == "submit" {
					patternRefs := newUIAOwnedRefs()
					pat, pe := getCurrentPatternAs(comCall, el, uiaInvoke, &iidUIAutomationInvokePattern)
					patternRefs.own(pat)
					patternRefs.close()
					if pe != nil {
						continue
					}
				}
				actionable = append(actionable, el)
			}
			if len(actionable) > 1 {
				return fmt.Errorf("UI Automation target is ambiguous")
			}
			if len(actionable) == 0 {
				return nil
			}
			if ctx.Err() != nil {
				return ctx.Err()
			}
			if !(windowsDriver{}).ValidateWindow(ctx, id) {
				return fmt.Errorf("window became stale before action")
			}
			if action == "focus" {
				foreground, _, _ := getForegroundWindowUIA.Call()
				if foreground != hwnd || !(windowsDriver{}).ValidateWindow(ctx, id) {
					return fmt.Errorf("requested window is not current foreground")
				}
				_, e = comCall(actionable[0], 3)
				if e != nil {
					return fmt.Errorf("UI Automation focus failed")
				}
				foreground, _, _ = getForegroundWindowUIA.Call()
				if foreground != hwnd || !readUIABool(actionable[0], uiaHasKeyboardFocus) {
					return fmt.Errorf("requested UI Automation target did not acquire focus in the target window")
				}
			} else if action == "click" || action == "submit" {
				foreground, _, _ := getForegroundWindowUIA.Call()
				if foreground != hwnd || !(windowsDriver{}).ValidateWindow(ctx, id) {
					return fmt.Errorf("requested window is not current foreground")
				}
				patternRefs := newUIAOwnedRefs()
				pat, patternErr := getCurrentPatternAs(comCall, actionable[0], uiaInvoke, &iidUIAutomationInvokePattern)
				patternRefs.own(pat)
				if patternErr != nil {
					patternRefs.close()
					return fmt.Errorf("UI Automation invocation unavailable")
				}
				defer patternRefs.close()
				foreground, _, _ = getForegroundWindowUIA.Call()
				if foreground != hwnd || !(windowsDriver{}).ValidateWindow(ctx, id) {
					return fmt.Errorf("requested window is not current foreground")
				}
				_, e = comCall(pat, 3)
				if e != nil {
					return fmt.Errorf("UI Automation invocation failed")
				}
			} else if action == "double_click" {
				if e = doubleClickUIA(ctx, hwnd, id, actionable[0]); e != nil {
					return e
				}
			} else {
				return fmt.Errorf("unsupported UI Automation action")
			}
			applied = true
			return nil
		})
		if e != nil {
			return e
		}
		if applied {
			return nil
		}
		if rawFound {
			return fmt.Errorf("UI Automation target is not actionable")
		}
	}
	return fmt.Errorf("UI Automation target not found")
}

func doubleClickUIA(ctx context.Context, hwnd uintptr, id string, el unsafe.Pointer) error {
	getRect := func() (uiaRect, error) {
		v, ok := readUIAProperty(el, uiaBounds)
		if !ok {
			return uiaRect{}, fmt.Errorf("target bounds unavailable")
		}
		defer clearUIAVariant(&v)
		if v.Vt != (vtArray | vtR8) {
			return uiaRect{}, fmt.Errorf("target bounds unavailable")
		}
		a := *(*unsafe.Pointer)(unsafe.Pointer(&v.Value[0]))
		if a == nil {
			return uiaRect{}, fmt.Errorf("target bounds unavailable")
		}
		var lo, hi int32
		if r, _, _ := safeArrayGetLB.Call(uintptr(a), 1, uintptr(unsafe.Pointer(&lo))); int32(r) < 0 {
			return uiaRect{}, fmt.Errorf("target bounds unavailable")
		}
		if r, _, _ := safeArrayGetUB.Call(uintptr(a), 1, uintptr(unsafe.Pointer(&hi))); int32(r) < 0 || hi-lo != 3 {
			return uiaRect{}, fmt.Errorf("target bounds unavailable")
		}
		var v4 [4]float64
		for i := 0; i < 4; i++ {
			idx := lo + int32(i)
			r, _, _ := safeArrayGetElement.Call(uintptr(a), uintptr(unsafe.Pointer(&idx)), uintptr(unsafe.Pointer(&v4[i])))
			if int32(r) < 0 {
				return uiaRect{}, fmt.Errorf("target bounds unavailable")
			}
		}
		return uiaRect{int32(v4[0]), int32(v4[1]), int32(v4[0] + v4[2]), int32(v4[1] + v4[3])}, nil
	}
	b, e := getRect()
	if e != nil {
		return e
	}
	var w uiaRect
	if ok, _, _ := getWindowRectUIA.Call(hwnd, uintptr(unsafe.Pointer(&w))); ok == 0 {
		return fmt.Errorf("window bounds unavailable")
	}
	if b.Right <= b.Left || b.Bottom <= b.Top || b.Left < w.Left || b.Top < w.Top || b.Right > w.Right || b.Bottom > w.Bottom {
		return fmt.Errorf("target is outside its window")
	}
	x := b.Left + (b.Right-b.Left)/2
	y := b.Top + (b.Bottom-b.Top)/2
	left, _, _ := getSystemMetrics.Call(metricVirtualLeft)
	top, _, _ := getSystemMetrics.Call(metricVirtualTop)
	width, _, _ := getSystemMetrics.Call(metricVirtualWidth)
	height, _, _ := getSystemMetrics.Call(metricVirtualHeight)
	if width == 0 || height == 0 || x < int32(left) || y < int32(top) || uintptr(x-int32(left)) >= width || uintptr(y-int32(top)) >= height {
		return fmt.Errorf("target is outside desktop bounds")
	}
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if !(windowsDriver{}).ValidateWindow(ctx, id) {
		return fmt.Errorf("window became stale before action")
	}
	foreground, _, _ := getForegroundWindowUIA.Call()
	if foreground != hwnd {
		return fmt.Errorf("requested window is not current foreground")
	}
	if r, _, _ := setCursorPos.Call(uintptr(uint32(x)), uintptr(uint32(y))); r == 0 {
		return fmt.Errorf("target could not be reached")
	}
	cur, e := getRect()
	if e != nil || cur != b || !readUIABool(el, uiaEnabled) || readUIABool(el, uiaOffscreen) {
		return fmt.Errorf("target changed before click")
	}
	var point uiaPoint
	if r, _, _ := getCursorPosUIA.Call(uintptr(unsafe.Pointer(&point))); r == 0 || point.X != x || point.Y != y {
		return fmt.Errorf("target location could not be verified")
	}
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if !(windowsDriver{}).ValidateWindow(ctx, id) {
		return fmt.Errorf("window became stale before click")
	}
	foreground, _, _ = getForegroundWindowUIA.Call()
	if foreground != hwnd {
		return fmt.Errorf("requested window is not current foreground")
	}
	inputs := [4][40]byte{}
	flags := [4]uint32{2, 4, 2, 4}
	for i := range inputs {
		*(*uint32)(unsafe.Pointer(&inputs[i][20])) = flags[i]
	}
	if n, _, _ := sendInput.Call(4, uintptr(unsafe.Pointer(&inputs[0][0])), 40); n != 4 {
		return fmt.Errorf("double-click input failed")
	}
	return nil
}
func readUIAProperty(el unsafe.Pointer, p int32) (uiaVariant, bool) {
	var v uiaVariant
	_, e := comCall(el, 10, uintptr(p), uintptr(unsafe.Pointer(&v)))
	if e != nil {
		clearUIAVariant(&v)
		return uiaVariant{}, false
	}
	return v, true
}
func clearUIAVariant(v *uiaVariant) { variantClear.Call(uintptr(unsafe.Pointer(v))) }
func readUIAString(el unsafe.Pointer, p int32) string {
	v, ok := readUIAProperty(el, p)
	if !ok {
		return ""
	}
	defer clearUIAVariant(&v)
	if v.Vt != vtBSTR {
		return ""
	}
	b := *(*unsafe.Pointer)(unsafe.Pointer(&v.Value[0]))
	if b == nil {
		return ""
	}
	n, _, _ := sysStringLen.Call(uintptr(b))
	if n > 4096 {
		n = 4096
	}
	return syscall.UTF16ToString(unsafe.Slice((*uint16)(b), int(n)))
}
func readUIAInt(el unsafe.Pointer, p int32) int32 {
	v, ok := readUIAProperty(el, p)
	if !ok {
		return 0
	}
	defer clearUIAVariant(&v)
	if v.Vt != vtI4 {
		return 0
	}
	return *(*int32)(unsafe.Pointer(&v.Value[0]))
}
func readUIABool(el unsafe.Pointer, p int32) bool {
	v, ok := readUIAProperty(el, p)
	if !ok {
		return false
	}
	defer clearUIAVariant(&v)
	return v.Vt == vtBool && *(*int16)(unsafe.Pointer(&v.Value[0])) != 0
}
