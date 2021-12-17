package main

import (
	"github.com/jurgen-kluft/xcode"
	pkg "github.com/jurgen-kluft/xsuperalloc/package"
)

func main() {
	xcode.Init()
	xcode.Generate(pkg.GetPackage())
}
