package csuperalloc

import (
	callocator "github.com/jurgen-kluft/callocator/package"
	cbase "github.com/jurgen-kluft/cbase/package"
	denv "github.com/jurgen-kluft/ccode/denv"
	ccore "github.com/jurgen-kluft/ccore/package"
	cunittest "github.com/jurgen-kluft/cunittest/package"
	cvmem "github.com/jurgen-kluft/cvmem/package"
)

const (
	repo_path = "github.com\\jurgen-kluft\\"
	repo_name = "callocator"
)

func GetPackage() *denv.Package {
	name := repo_name

	// dependencies
	cunittestpkg := cunittest.GetPackage()
	cbasepkg := cbase.GetPackage()
	ccorepkg := ccore.GetPackage()
	cvmempkg := cvmem.GetPackage()
	callocatorpkg := callocator.GetPackage()

	// main package
	mainpkg := denv.NewPackage(repo_path, repo_name)
	mainpkg.AddPackage(cunittestpkg)
	mainpkg.AddPackage(cbasepkg)
	mainpkg.AddPackage(ccorepkg)
	mainpkg.AddPackage(cvmempkg)
	mainpkg.AddPackage(callocatorpkg)

	// main library
	mainlib := denv.SetupCppLibProject(mainpkg, name)
	mainlib.AddDependencies(ccorepkg.GetMainLib()...)
	mainlib.AddDependencies(cbasepkg.GetMainLib()...)
	mainlib.AddDependencies(cvmempkg.GetMainLib()...)
	mainlib.AddDependencies(callocatorpkg.GetMainLib()...)

	// test library
	testlib := denv.SetupCppTestLibProject(mainpkg, name)
	testlib.AddDependencies(ccorepkg.GetTestLib()...)
	testlib.AddDependencies(cbasepkg.GetTestLib()...)
	testlib.AddDependencies(cvmempkg.GetTestLib()...)
	testlib.AddDependencies(callocatorpkg.GetTestLib()...)

	// unittest project
	maintest := denv.SetupCppTestProject(mainpkg, name)
	maintest.AddDependencies(cunittestpkg.GetMainLib()...)
	maintest.AddDependency(mainlib)

	mainpkg.AddMainLib(mainlib)
	mainpkg.AddTestLib(testlib)
	mainpkg.AddUnittest(maintest)
	return mainpkg
}
