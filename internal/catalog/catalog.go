package catalog

import (
	"encoding/json"
	"sort"
)

type Descriptor struct {
	Name        string          `json:"name"`
	Description string          `json:"description"`
	Risk        string          `json:"risk"`
	Profile     string          `json:"profile,omitempty"`
	InputSchema json.RawMessage `json:"input_schema,omitempty"`
}

const (
	ChatOrchestratorProfile = "computer.chat-orchestrator-v1"
)

var items = []Descriptor{
	{Name: "screen.capture", Description: "Capture the primary display without persisting the image", Risk: "sensitive_read"},
	{Name: "pointer.move", Description: "Move the pointer in primary-display pixel coordinates", Risk: "interaction"},
	{Name: "pointer.click", Description: "Click in primary-display pixel coordinates", Risk: "interaction"},
	{Name: "keyboard.type", Description: "Type literal text into the focused desktop window", Risk: "sensitive_write", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"required":["text"],"properties":{"text":{"type":"string","minLength":1,"maxLength":4096}}}`)},
	{Name: "keyboard.key", Description: "Press a supported key in the focused desktop window", Risk: "interaction", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"required":["key"],"properties":{"key":{"type":"string","enum":["ENTER","ESC","TAB","SPACE","BACKSPACE","DELETE","ARROW_UP","ARROW_DOWN","ARROW_LEFT","ARROW_RIGHT","HOME","END"]}}}`)},
	{Name: "clipboard.read", Description: "Read plain-text clipboard contents", Risk: "sensitive_read"},
	{Name: "clipboard.write", Description: "Replace plain-text clipboard contents", Risk: "sensitive_write"},
	{Name: "window.list", Description: "List visible top-level windows and their titles", Risk: "sensitive_read", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false}`)},
	{Name: "browser.status", Description: "Report visible browser presence without returning window titles or URLs", Risk: "sensitive_read", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false}`)},
	{Name: "window.focus", Description: "Activate a current top-level window", Risk: "interaction", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"required":["window_id"],"properties":{"window_id":{"type":"string","minLength":1,"maxLength":256}}}`)},
	{Name: "ui.inspect", Description: "Inspect a bounded UI Automation tree in a desktop window", Risk: "sensitive_read", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"properties":{"window_id":{"type":"string","minLength":1,"maxLength":256}}}`)},
	{Name: "ui.focus", Description: "Focus a uniquely selected UI Automation control", Risk: "interaction", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"required":["window_id","target"],"properties":{"window_id":{"type":"string","minLength":1,"maxLength":256},"target":{"type":"string","minLength":1,"maxLength":512}}}`)},
	{Name: "ui.invoke", Description: "Invoke a supported UI Automation control action", Risk: "interaction", Profile: ChatOrchestratorProfile, InputSchema: json.RawMessage(`{"type":"object","additionalProperties":false,"required":["window_id","target","action"],"properties":{"window_id":{"type":"string","minLength":1,"maxLength":256},"target":{"type":"string","minLength":1,"maxLength":512},"action":{"type":"string","enum":["click","double_click","submit"]}}}`)},
	{Name: "browser.navigate", Description: "Navigate the endpoint-owned managed browser to an HTTP or HTTPS URL", Risk: "external_navigation"},
	{Name: "browser.snapshot", Description: "Return a bounded structured snapshot of the managed browser", Risk: "sensitive_read"},
	{Name: "browser.query", Description: "Query bounded visible text in the managed browser", Risk: "sensitive_read"},
	{Name: "browser.click", Description: "Click a managed browser snapshot reference", Risk: "interaction"},
	{Name: "browser.fill", Description: "Fill a managed browser form field", Risk: "sensitive_write"},
	{Name: "browser.select", Description: "Select a managed browser option", Risk: "interaction"},
	{Name: "browser.tabs", Description: "List tabs in the managed browser", Risk: "sensitive_read"},
	{Name: "browser.back", Description: "Navigate back in the managed browser", Risk: "external_navigation"},
	{Name: "browser.screenshot", Description: "Capture a bounded managed browser screenshot", Risk: "sensitive_read"},
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

func Profiles() map[string][]string {
	profiles := map[string][]string{ChatOrchestratorProfile: {}}
	for _, item := range items {
		if item.Profile != "" {
			profiles[item.Profile] = append(profiles[item.Profile], item.Name)
		}
	}
	for profile := range profiles {
		sort.Strings(profiles[profile])
	}
	return profiles
}

func ProfileDescriptors() map[string][]Descriptor {
	profiles := map[string][]Descriptor{ChatOrchestratorProfile: {}}
	for _, item := range List() {
		if item.Profile != "" {
			profiles[item.Profile] = append(profiles[item.Profile], item)
		}
	}
	return profiles
}
