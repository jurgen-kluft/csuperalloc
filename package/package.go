package csuperalloc

import (
	callocator "github.com/jurgen-kluft/callocator/package"
	cbase "github.com/jurgen-kluft/cbase/package"
	denv "github.com/jurgen-kluft/ccode/denv"
	ccore "github.com/jurgen-kluft/ccore/package"
	cunittest "github.com/jurgen-kluft/cunittest/package"
	cvmem "github.com/jurgen-kluft/cvmem/package"
)

// GetPackage returns the package object of 'csuperalloc'
func GetPackage() *denv.Package {
	// Dependencies
	cunittestpkg := cunittest.GetPackage()
	cbasepkg := cbase.GetPackage()
	ccorepkg := ccore.GetPackage()
	cvmempkg := cvmem.GetPackage()
	callocatorpkg := callocator.GetPackage()

	// The main (csuperalloc) package
	mainpkg := denv.NewPackage("csuperalloc")
	mainpkg.AddPackage(cunittestpkg)
	mainpkg.AddPackage(cbasepkg)
	mainpkg.AddPackage(ccorepkg)
	mainpkg.AddPackage(cvmempkg)
	mainpkg.AddPackage(callocatorpkg)

	// 'csuperalloc' library
	mainlib := denv.SetupDefaultCppLibProject("csuperalloc", "github.com\\jurgen-kluft\\csuperalloc")
	mainlib.Dependencies = append(mainlib.Dependencies, ccorepkg.GetMainLib())
	mainlib.Dependencies = append(mainlib.Dependencies, cbasepkg.GetMainLib())
	mainlib.Dependencies = append(mainlib.Dependencies, cvmempkg.GetMainLib())
	mainlib.Dependencies = append(mainlib.Dependencies, callocatorpkg.GetMainLib())

	// 'csuperalloc' unittest project
	maintest := denv.SetupDefaultCppTestProject("csuperalloc"+"_test", "github.com\\jurgen-kluft\\csuperalloc")
	maintest.Dependencies = append(maintest.Dependencies, cunittestpkg.GetMainLib())
	maintest.Dependencies = append(maintest.Dependencies, mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
