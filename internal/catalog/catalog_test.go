package catalog

import (
	"strings"
	"testing"
)

func TestFrozenCapabilityProfiles(t *testing.T) {
	profiles := Profiles()
	wantChat := []string{"browser.status", "keyboard.key", "keyboard.type", "ui.focus", "ui.inspect", "ui.invoke", "window.focus", "window.list"}
	assertNames := func(profile string, want []string) {
		t.Helper()
		got := profiles[profile]
		if len(got) != len(want) {
			t.Fatalf("%s has %v; want %v", profile, got, want)
		}
		for i := range want {
			if got[i] != want[i] {
				t.Fatalf("%s has %v; want %v", profile, got, want)
			}
		}
	}
	assertNames(ChatOrchestratorProfile, wantChat)
	if len(profiles) != 1 {
		t.Fatalf("unsupported profiles advertised: %v", profiles)
	}
	descriptors := ProfileDescriptors()
	if len(descriptors) != 1 || len(descriptors[ChatOrchestratorProfile]) != len(wantChat) {
		t.Fatalf("unsupported profile descriptors advertised: %v", descriptors)
	}
	for _, item := range List() {
		if strings.HasPrefix(item.Name, "browser.") && item.Name != "browser.status" && (item.Profile != "" || len(item.InputSchema) != 0) {
			t.Fatalf("unimplemented managed browser metadata advertised: %+v", item)
		}
	}
}
