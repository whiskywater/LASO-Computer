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
