package capabilities

import (
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/Registered-Agent-Attorney/LASO-Computer/internal/platform"
)

type browserStatusDesktop struct {
	platform.Desktop
	windows []platform.Window
}

func (d browserStatusDesktop) Windows(context.Context) ([]platform.Window, error) {
	return d.windows, nil
}

func TestBrowserStatusDoesNotReturnWindowTitles(t *testing.T) {
	desktop := browserStatusDesktop{windows: []platform.Window{
		{ID: "1", Title: "Private matter title - Microsoft Edge", Active: true},
		{ID: "2", Title: "Terminal", Active: false},
		{ID: "3", Title: "Knowledge base", Active: false},
	}}
	result, err := (handlers{desktop: desktop}).browserStatus(context.Background(), json.RawMessage(`{}`))
	if err != nil {
		t.Fatal(err)
	}
	var decoded struct {
		WindowCount   int             `json:"window_count"`
		BrowserStatus map[string]bool `json:"browser_status"`
	}
	if err := json.Unmarshal(result, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.WindowCount != 3 || !decoded.BrowserStatus["browser_visible"] ||
		!decoded.BrowserStatus["active_browser_visible"] {
		t.Fatalf("unexpected browser status: %s", result)
	}
	if strings.Contains(string(result), "Private matter title") || strings.Contains(string(result), "Terminal") {
		t.Fatalf("window title escaped into browser status: %s", result)
	}
}

func TestBrowserStatusRejectsUnknownArguments(t *testing.T) {
	desktop := browserStatusDesktop{}
	if _, err := (handlers{desktop: desktop}).browserStatus(context.Background(), json.RawMessage(`{"url":"https://example.com"}`)); err == nil {
		t.Fatal("browser.status accepted an argument")
	}
}

func TestBrowserStatusDoesNotTreatKnowledgeAsBrowser(t *testing.T) {
	desktop := browserStatusDesktop{windows: []platform.Window{{ID: "1", Title: "Knowledge base"}}}
	result, err := (handlers{desktop: desktop}).browserStatus(context.Background(), json.RawMessage(`{}`))
	if err != nil {
		t.Fatal(err)
	}
	var decoded struct {
		BrowserStatus map[string]bool `json:"browser_status"`
	}
	if err := json.Unmarshal(result, &decoded); err != nil {
		t.Fatal(err)
	}
	if decoded.BrowserStatus["browser_visible"] {
		t.Fatalf("a non-browser title was misidentified: %s", result)
	}
}
