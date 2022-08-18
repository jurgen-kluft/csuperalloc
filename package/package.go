package csuperalloc

import (
	cbase "github.com/jurgen-kluft/cbase/package"
	centry "github.com/jurgen-kluft/centry/package"
	cunittest "github.com/jurgen-kluft/cunittest/package"
	cvmem "github.com/jurgen-kluft/cvmem/package"
	"github.com/jurgen-kluft/xcode/denv"
)

// GetPackage returns the package object of 'csuperalloc'
func GetPackage() *denv.Package {
	// Dependencies
	cunittestpkg := cunittest.GetPackage()
	centrypkg := centry.GetPackage()
	cvmempkg := cvmem.GetPackage()
	cbasepkg := cbase.GetPackage()

	// The main (csuperalloc) package
	mainpkg := denv.NewPackage("csuperalloc")
	mainpkg.AddPackage(cunittestpkg)
	mainpkg.AddPackage(centrypkg)
	mainpkg.AddPackage(cvmempkg)
	mainpkg.AddPackage(cbasepkg)

	// 'csuperalloc' library
	mainlib := denv.SetupDefaultCppLibProject("csuperalloc", "github.com\\jurgen-kluft\\csuperalloc")
	mainlib.Dependencies = append(mainlib.Dependencies, cvmempkg.GetMainLib(), cbasepkg.GetMainLib())

	// 'csuperalloc' unittest project
	maintest := denv.SetupDefaultCppTestProject("csuperalloc_test", "github.com\\jurgen-kluft\\csuperalloc")
	maintest.Dependencies = append(maintest.Dependencies, cunittestpkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, centrypkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, cbasepkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, cvmempkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
