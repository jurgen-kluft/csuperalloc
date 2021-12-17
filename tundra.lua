local GlobExtension = require("tundra.syntax.glob")
Build {
	ReplaceEnv = {
		OBJECTROOT = "target",
	},
	Env = {
		CPPDEFS = {
			{ "TARGET_PC_DEV_DEBUG", "TARGET_PC", "PLATFORM_64BIT"; Config = "win64-*-debug-dev" },
			{ "TARGET_PC_DEV_RELEASE", "TARGET_PC", "PLATFORM_64BIT"; Config = "win64-*-release-dev" },
			{ "TARGET_PC_TEST_DEBUG", "TARGET_PC", "PLATFORM_64BIT"; Config = "win64-*-debug-test" },
			{ "TARGET_PC_TEST_RELEASE", "TARGET_PC", "PLATFORM_64BIT"; Config = "win64-*-release-test" },
			{ "TARGET_MAC_DEV_DEBUG", "TARGET_MAC", "PLATFORM_64BIT"; Config = "macosx-*-debug-dev" },
			{ "TARGET_MAC_DEV_RELEASE", "TARGET_MAC", "PLATFORM_64BIT"; Config = "macosx-*-release-dev" },
			{ "TARGET_MAC_TEST_DEBUG", "TARGET_MAC", "PLATFORM_64BIT"; Config = "macosx-*-debug-test" },
			{ "TARGET_MAC_TEST_RELEASE", "TARGET_MAC", "PLATFORM_64BIT"; Config = "macosx-*-release-test" },
		},
	},
	Units = function ()
		-- Recursively globs for source files relevant to current build-id
		local function SourceGlobCommon(dir)
			return FGlob {
				Dir = dir,
				Extensions = { ".c", ".cpp", ".s", ".asm" },
				Filters = {
					{ Pattern = "_[Ww]in32"; Config = "ignore" },
					{ Pattern = "_[Mm]ac"; Config = "ignore" },
				}
			}
		end
		local function SourceGlobPlatform(dir)
			return FGlob {
				Dir = dir,
				Extensions = { ".c", ".cpp", ".s", ".asm" },
				Filters = {
					{ Pattern = "_[Ww]in32"; Config = "win64-*-*-*" },
					{ Pattern = "_[Mm]ac"; Config = "macosx-*-*-*" },
					{ Pattern = ""; Config = "ignore" },
				}
			}
		end
		local xunittest_library = StaticLibrary {
			Name = "xunittest",
			Config = "*-*-*-test",
			Sources = { SourceGlobCommon("../xunittest/source/main/cpp"), SourceGlobPlatform("../xunittest/source/main/cpp") },
			Includes = { "../xunittest/source/main/include" },
		}
		local xentry_library = StaticLibrary {
			Name = "xentry",
			Config = "*-*-*-*",
			Sources = { SourceGlobCommon("../xentry/source/main/cpp"), SourceGlobPlatform("../xentry/source/main/cpp") },
			Includes = { "../xentry/source/main/include" },
		}
		local xbase_library = StaticLibrary {
			Name = "xbase",
			Config = "*-*-*-*",
			Sources = { SourceGlobCommon("../xbase/source/main/cpp"), SourceGlobPlatform("../xbase/source/main/cpp") },
			Includes = { "../xbase/source/main/include","../xunittest/source/main/include" },
		}
		local xvmem_library = StaticLibrary {
			Name = "xvmem",
			Config = "*-*-*-*",
			Sources = { SourceGlobCommon("source/main/cpp"), SourceGlobPlatform("source/main/cpp") },
			Includes = { "../xvmem/source/main/include","../xbase/source/main/include" },
		}
		local xsuperalloc_library = StaticLibrary {
			Name = "xsuperalloc",
			Config = "*-*-*-*",
			Sources = { SourceGlobCommon("source/main/cpp"), SourceGlobPlatform("source/main/cpp") },
			Includes = { "../xsuperalloc/source/main/include","../xbase/source/main/include" },
		}
		local unittest = Program {
			Name = "xsuperalloc_test",
			Config = "*-*-*-test",
			Sources = { SourceGlobCommon("source/test/cpp"), SourceGlobPlatform("source/test/cpp") },
			Includes = { "source/main/include","source/test/include","../xunittest/source/main/include","../xentry/source/main/include","../xbase/source/main/include","../xvmem/source/main/include","../xsuperalloc/source/main/include" },
			Depends = { xunittest_library,xentry_library,xbase_library,xvmem_library,xsuperalloc_library },
		}
		Default(unittest)
	end,
	Configs = {
		Config {
			Name = "macosx-clang",
			Env = {
			PROGOPTS = { "-lc++" },
			CXXOPTS = {
				"-std=c++11",
				"-arch x86_64",
				"-g",
				"-Wno-new-returns-null",
				"-Wno-missing-braces",
				"-Wno-unused-function",
				"-Wno-unused-variable",
				"-Wno-unused-result",
				"-Wno-write-strings",
				"-Wno-c++11-compat-deprecated-writable-strings",
				"-Wno-null-dereference",
				"-Wno-format",
				"-fno-strict-aliasing",
				"-fno-omit-frame-pointer",
			},
		},
		DefaultOnHost = "macosx",
		Tools = { "clang" },
		},
		Config {
			ReplaceEnv = {
				OBJECTROOT = "target",
			},
			Name = "linux-gcc",
			DefaultOnHost = "linux",
			Tools = { "gcc" },
		},
		Config {
			ReplaceEnv = {
				OBJECTROOT = "target",
			},
			Name = "win64-msvc",
			Env = {
				PROGOPTS = { "/SUBSYSTEM:CONSOLE" },
				CXXOPTS = { },
			},
			DefaultOnHost = "windows",
			Tools = { "msvc-vs2017" },
		},
	},
	SubVariants = { "dev", "test" },
}
