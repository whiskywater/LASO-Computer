package identity

import "testing"

func TestIdentityPersistsWithoutHardwareDerivation(t *testing.T) {
	dir := t.TempDir()
	first, err := LoadOrCreate(dir)
	if err != nil {
		t.Fatal(err)
	}
	second, err := LoadOrCreate(dir)
	if err != nil {
		t.Fatal(err)
	}
	if first != second || !valid(first) {
		t.Fatal("identity was not persisted as a random local ID")
	}
}
