//go:build windows

package platform

import (
	"context"
	"reflect"
	"testing"
	"unsafe"
)

func TestBoundedUIATraversalDepthAndTruncation(t *testing.T) {
	tree := map[string][]string{"root": {"child"}, "child": {"grandchild"}}
	children := func(node string) ([]string, error) { return tree[node], nil }
	collect := func(nodes []uiaDepthNode[string], truncated bool) error {
		want := []uiaDepthNode[string]{{value: "root", depth: 0}, {value: "child", depth: 1}, {value: "grandchild", depth: 2}}
		if truncated || !reflect.DeepEqual(nodes, want) {
			t.Errorf("tree traversal=%+v truncated=%v; want=%+v, false", nodes, truncated, want)
		}
		return nil
	}
	if err := boundedUIATraversal(context.Background(), "root", 2, 10, children, func(string) bool { return true }, collect); err != nil {
		t.Fatal(err)
	}
	var got []uiaDepthNode[string]
	var wasTruncated bool
	collectBounded := func(nodes []uiaDepthNode[string], truncated bool) error {
		got = nodes
		wasTruncated = truncated
		return nil
	}
	if err := boundedUIATraversal(context.Background(), "root", 0, 10, children, func(string) bool { return true }, collectBounded); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].value != "root" || got[0].depth != 0 || !wasTruncated {
		t.Fatalf("depth-bound traversal=%+v truncated=%v", got, wasTruncated)
	}
	if err := boundedUIATraversal(context.Background(), "root", 2, 1, children, func(string) bool { return true }, collectBounded); err != nil {
		t.Fatal(err)
	}
	if len(got) != 1 || got[0].value != "root" || got[0].depth != 0 || !wasTruncated {
		t.Fatalf("node-bound traversal=%+v truncated=%v", got, wasTruncated)
	}
}

func TestUIAOwnedRefsReleaseEachAcquisitionOnce(t *testing.T) {
	counts := map[unsafe.Pointer]int{}
	refs := &uiaOwnedRefs{release: func(p unsafe.Pointer) { counts[p]++ }}
	first := unsafe.Pointer(new(byte))
	shared := unsafe.Pointer(new(byte))
	refs.own(first)
	refs.own(shared)
	// A second successful COM call may return the same interface address with a
	// second AddRef. It is a distinct owned reference and must be released too.
	refs.own(shared)
	refs.close()
	refs.close()
	if counts[first] != 1 || counts[shared] != 2 || len(refs.refs) != 0 {
		t.Fatalf("COM release counts first=%d shared=%d remaining=%d", counts[first], counts[shared], len(refs.refs))
	}
}

func TestGetCurrentPatternAsUsesDocumentedSlotAndSignature(t *testing.T) {
	element := unsafe.Pointer(new(byte))
	called := false
	pattern, err := getCurrentPatternAs(func(object unsafe.Pointer, slot uintptr, args ...uintptr) (uintptr, error) {
		called = true
		if object != element {
			t.Fatalf("object=%p want=%p", object, element)
		}
		if uiaElementGetCurrentPatternAsSlot != 14 {
			t.Fatalf("documented GetCurrentPatternAs slot=%d want=14", uiaElementGetCurrentPatternAsSlot)
		}
		if slot != uiaElementGetCurrentPatternAsSlot {
			t.Fatalf("slot=%d want=%d", slot, uiaElementGetCurrentPatternAsSlot)
		}
		if len(args) != 3 || args[0] != uiaInvoke || args[1] != uintptr(unsafe.Pointer(&iidUIAutomationInvokePattern)) || args[2] == 0 {
			t.Fatalf("GetCurrentPatternAs args=%v", args)
		}
		return 0, nil
	}, element, uiaInvoke, &iidUIAutomationInvokePattern)
	if !called {
		t.Fatal("GetCurrentPatternAs COM call was not attempted")
	}
	if err == nil || pattern != nil {
		t.Fatalf("nil pattern result pattern=%p err=%v", pattern, err)
	}
}
