package catalog

import "sort"

type Descriptor struct {
	Name        string `json:"name"`
	Description string `json:"description"`
	Risk        string `json:"risk"`
}

var items = []Descriptor{
	{Name: "screen.capture", Description: "Capture the primary display without persisting the image", Risk: "sensitive_read"},
	{Name: "pointer.move", Description: "Move the pointer in primary-display pixel coordinates", Risk: "interaction"},
	{Name: "pointer.click", Description: "Click in primary-display pixel coordinates", Risk: "interaction"},
	{Name: "keyboard.type", Description: "Type literal text into the focused application", Risk: "sensitive_write"},
	{Name: "keyboard.key", Description: "Press a common key, optionally with modifiers", Risk: "interaction"},
	{Name: "clipboard.read", Description: "Read plain-text clipboard contents", Risk: "sensitive_read"},
	{Name: "clipboard.write", Description: "Replace plain-text clipboard contents", Risk: "sensitive_write"},
	{Name: "window.list", Description: "List visible top-level windows and their titles", Risk: "sensitive_read"},
	{Name: "window.focus", Description: "Activate a visible top-level window by its local ID", Risk: "interaction"},
	{Name: "browser.navigate", Description: "Open an HTTP or HTTPS URL in the default browser", Risk: "external_navigation"},
	{Name: "shell.execute", Description: "Run an explicitly allowlisted executable with an argument vector", Risk: "high"},
}

func List() []Descriptor {
	result := append([]Descriptor(nil), items...)
	sort.Slice(result, func(i, j int) bool { return result[i].Name < result[j].Name })
	return result
}

func Known(name string) bool {
	for _, item := range items {
		if item.Name == name {
			return true
		}
	}
	return false
}
