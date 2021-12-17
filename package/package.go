package xsuperalloc

import (
	xbase "github.com/jurgen-kluft/xbase/package"
	"github.com/jurgen-kluft/xcode/denv"
	xentry "github.com/jurgen-kluft/xentry/package"
	xunittest "github.com/jurgen-kluft/xunittest/package"
	xvmem "github.com/jurgen-kluft/xvmem/package"
)

// GetPackage returns the package object of 'xsuperalloc'
func GetPackage() *denv.Package {
	// Dependencies
	xunittestpkg := xunittest.GetPackage()
	xentrypkg := xentry.GetPackage()
	xvmempkg := xvmem.GetPackage()
	xbasepkg := xbase.GetPackage()

	// The main (xsuperalloc) package
	mainpkg := denv.NewPackage("xsuperalloc")
	mainpkg.AddPackage(xunittestpkg)
	mainpkg.AddPackage(xentrypkg)
	mainpkg.AddPackage(xvmempkg)
	mainpkg.AddPackage(xbasepkg)

	// 'xsuperalloc' library
	mainlib := denv.SetupDefaultCppLibProject("xsuperalloc", "github.com\\jurgen-kluft\\xsuperalloc")
	mainlib.Dependencies = append(mainlib.Dependencies, xvmempkg.GetMainLib(), xbasepkg.GetMainLib())

	// 'xsuperalloc' unittest project
	maintest := denv.SetupDefaultCppTestProject("xsuperalloc_test", "github.com\\jurgen-kluft\\xsuperalloc")
	maintest.Dependencies = append(maintest.Dependencies, xunittestpkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, xentrypkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, xbasepkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, xvmempkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
