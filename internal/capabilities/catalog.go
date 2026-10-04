package capabilities

import "github.com/Registered-Agent-Attorney/LASO-Computer/internal/catalog"

type Descriptor = catalog.Descriptor

func Catalog() []Descriptor  { return catalog.List() }
func Known(name string) bool { return catalog.Known(name) }
