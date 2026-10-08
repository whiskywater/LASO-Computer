package platform

import (
	"context"
	"image"
)

type Window struct {
	ID     string `json:"id"`
	Title  string `json:"title"`
	Active bool   `json:"active"`
}

type Display struct {
	Width  int    `json:"width"`
	Height int    `json:"height"`
	Name   string `json:"name"`
}

// UISelector deliberately supports exact, bounded UIA properties only. An
// action must resolve to exactly one element; it never accepts a query script.
type UIElement struct {
	Name                  string `json:"name"`
	NameTruncated         bool   `json:"name_truncated"`
	AutomationID          string `json:"automation_id"`
	AutomationIDTruncated bool   `json:"automation_id_truncated"`
	ControlType           int32  `json:"control_type"`
	Enabled               bool   `json:"enabled"`
	Offscreen             bool   `json:"offscreen"`
	Depth                 int    `json:"depth"`
}

// UIAutomation is optional so non-Windows and headless endpoints fail closed.
type UIAutomation interface {
	Inspect(context.Context, string, int, int) ([]UIElement, bool, error)
	FocusElement(context.Context, string, string) error
	InvokeElement(context.Context, string, string, string) error
	ValidateWindow(context.Context, string) bool
}

type Desktop interface {
	Available() map[string]bool
	Capture(context.Context) (image.Image, Display, error)
	Move(context.Context, int, int) error
	Click(context.Context, int, int, string, int) error
	Type(context.Context, string) error
	Key(context.Context, string, []string) error
	Windows(context.Context) ([]Window, error)
	Focus(context.Context, string) error
	ClipboardRead(context.Context) (string, error)
	ClipboardWrite(context.Context, string) error
	OpenURL(context.Context, string) error
}
