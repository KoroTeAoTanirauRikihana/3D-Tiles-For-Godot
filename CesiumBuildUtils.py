# This file contains utility functions to build CesiumForGodot in SCons
import subprocess
import os
import fnmatch
import shutil
import sys

from SCons.Script import Dir

ROOT_DIR_MODULE = "#modules/cesium_godot"

ROOT_DIR_EXT = "#cesium_godot"

BINDINGS_DIR = "#godot-cpp"

CESIUM_MODULE_DEF = "CESIUM_GD_MODULE"

CESIUM_EXT_DEF = "CESIUM_GD_EXT"

CESIUM_NATIVE_DIR_EXT = "#cesium_godot/native"

CESIUM_NATIVE_DIR_MODULE = "#modules/cesium_godot/native"

OS_WIN = "nt"

OS_LINUX = "posix"

# sys.platform value for macOS (os.name returns 'posix' for both Linux and macOS)
PLATFORM_MACOS = "darwin"

STATIC_TRIPLET = "x64-windows-static"

RELEASE_CONFIG = "Release"

ezvcpkgFoundPath: str = ""


def get_compile_flags():
    if os.name == OS_WIN:
        return ["/std:c++20", "/Zc:__cplusplus", "/utf-8", "/bigobj"]
    elif sys.platform == PLATFORM_MACOS:
        return ["-std=c++20", "-fexceptions", "-fPIC"]
    elif os.name == OS_LINUX:
        return ["-std=c++20", "-fexceptions", "-fpermissive", "-fPIC"]


def get_linker_flags():
    if os.name == OS_WIN:
        return ["/IGNORE:4217"]
    return []


def is_extension_target(argsDict) -> bool:
    return get_compile_target_definition(argsDict) == CESIUM_EXT_DEF


def get_curl_lib_name() -> str:
    if os.name == OS_WIN:
        return "libcurl"
    return "curl"


def generate_precision_symbols(argsDict, env):
    print("Generating double precision compile symbols")
    desiredPrecision = argsDict.get("precision")
    if desiredPrecision == "double":
        env.Append(CPPDEFINES=["REAL_T_IS_DOUBLE"])


def get_compile_target_definition(argsDict) -> str:
    # Get the format (default is extension)
    global currentRootDir
    compileTarget = argsDict.get("compileTarget", CESIUM_EXT_DEF)
    if compileTarget == "module":
        print("[CESIUM] - Compiling Cesium For Godot as an engine module...")
        currentRootDir = ROOT_DIR_MODULE
        return CESIUM_MODULE_DEF
    if compileTarget == "" or compileTarget == "extension":
        print("[CESIUM] - Compiling Cesium For Godot as a GDExtension")
        currentRootDir = ROOT_DIR_EXT
        return CESIUM_EXT_DEF

    print("[CESIUM] - Compile target not recognized, options are: module / extension")
    exit(1)


def link_abseil_libs(env):
    foundLibs: list[SCons.Node.FS.File] = env.Glob(
        f"{find_ezvcpkg_path()}/packages/abseil_{determine_triplet()}/lib/*absl*.a"
    )

    # Dark magic to strip the lib prefix and the file extension
    foundLibs = [lib.name.replace("lib", "")[:-2] for lib in foundLibs]

    env.Append(LINKFLAGS=["-Wl,--start-group"], LIBS=foundLibs)

    env.Append(LINKFLAGS=["-Wl,--end-group"])

    env.Append(
        LINKFLAGS=["-Wl,--start-group"],
        LIBS=[
            "absl_log_internal_log_sink_set",
            "absl_log_globals",
            "absl_leak_check",
            "absl_log_internal_globals",
            "absl_log_internal_format",
            "absl_base",
            "absl_hash",
            "absl_city",
            "absl_low_level_hash",
            "absl_examine_stack",
            "absl_stacktrace",
            "absl_debugging_internal",
            "absl_synchronization",
            "absl_base",
            "absl_malloc_internal",
            "absl_int128",
            "absl_symbolize",
            "absl_kernel_timeout_internal",
            "absl_debugging_internal",
            "absl_demangle_internal",
            "absl_log_sink",
            "absl_demangle_rust",
            "absl_decode_rust_punycode",
            "absl_utf8_for_code_point",
        ],
    )
    env.Append(LINKFLAGS=["-Wl,--end-group"])


def clone_native_repo_if_needed():
    clone_repo_if_needed(
        ROOT_DIR_EXT + "/native",
        "Cesium Native",
        "https://github.com/CesiumGS/cesium-native.git",
        "v0.52.1",
        "9f6ae299e2709f866db52c4be29b6c31e10718c8",
    )


def clone_bindings_repo_if_needed():
    clone_repo_if_needed(
        BINDINGS_DIR,
        "Godot CPP Bindings",
        "https://github.com/godotengine/godot-cpp",
        "godot-4.1.4-stable",
        "4b0ee133274d67687b6003b8d5fdaf7b79cf4921",
    )


def clone_lite_html_if_needed():
    # clone_repo_if_needed(ROOT_DIR_EXT + "/third_party/lite-html", "Lite HTML",
    #                      "https://github.com/litehtml/litehtml.git", "v0.9", "6ca1ab0419e770e6d35a1ef690238773a1dafcee")
    pass


def build_litehtml(arch="arm64"):
    """Build litehtml from source for the given architecture."""
    third_party_dir = scons_to_abs_path(ROOT_DIR_EXT + "/third_party")
    source_dir = os.path.join(third_party_dir, "litehtml-src")
    output_dir = os.path.join(third_party_dir, "litehtml", "macos")

    # Check if already built
    if os.path.exists(os.path.join(output_dir, "liblitehtml.a")):
        print("litehtml already built for macOS, skipping...")
        return

    if not os.path.exists(source_dir):
        print("litehtml source not found at %s" % source_dir, file=sys.stderr)
        return

    print("Building litehtml from source for macOS...")

    build_dir = os.path.join(source_dir, "build-macos")
    os.makedirs(build_dir, exist_ok=True)
    os.makedirs(output_dir, exist_ok=True)

    prev_dir = os.getcwd()
    os.chdir(build_dir)

    # Configure with CMake
    result = subprocess.run([
        "cmake",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_OSX_ARCHITECTURES={arch}",
        "-DLITEHTML_BUILD_TESTING=OFF",
        ".."
    ])

    if result.returncode != 0:
        print("Failed to configure litehtml", file=sys.stderr)
        os.chdir(prev_dir)
        return

    # Build
    result = subprocess.run(["cmake", "--build", ".", "--config", "Release"])

    if result.returncode != 0:
        print("Failed to build litehtml", file=sys.stderr)
        os.chdir(prev_dir)
        return

    # Copy output libraries
    for lib in ["liblitehtml.a", "libgumbo.a"]:
        src = os.path.join(build_dir, lib)
        if not os.path.exists(src):
            # Try in subdirectories
            for root, dirs, files in os.walk(build_dir):
                if lib in files:
                    src = os.path.join(root, lib)
                    break
        if os.path.exists(src):
            shutil.copy2(src, output_dir)
            print(f"Copied {lib} to {output_dir}")

    os.chdir(prev_dir)
    print("litehtml build complete!")


def clone_repo_if_needed(
    targetDir: str, name: str, repoUrl: str, branch: str, acceptedCommitSHA: str
):
    print(f"Cloning {name} repo")
    repoDirectory = scons_to_abs_path(targetDir)
    if os.path.exists(repoDirectory):
        return
    subprocess.run(
        ["git", "clone", "--depth=1", "-b", branch, repoUrl, "--recursive", repoDirectory]
    )

    # Shouldn't we just rely on the repo tags?
    # prevDir: str = os.getcwd()
    # os.chdir(repoDirectory)
    # subprocess.run(["git", "reset", "--hard", acceptedCommitSHA])
    # os.chdir(prevDir)


# Configure with CMake
def configure_native(argumentsDict):
    print("Configuring Cesium Native")
    isExt = is_extension_target(argumentsDict)
    repoDirectory = CESIUM_NATIVE_DIR_EXT if isExt else CESIUM_NATIVE_DIR_MODULE
    repoDirectory = scons_to_abs_path(repoDirectory)
    os.chdir(repoDirectory)
    # 28 Oct 2026: cesium-native hard-codes its vcpkg commit inside ezvcpkg_fetch(COMMIT ...), and that
    # vcpkg asks msys2 for msys2-runtime-3.5.4-2, which msys2 has removed (404 on every mirror), so every
    # build stopped at configure. A cache variable cannot override an explicit COMMIT argument, so when
    # EZVCPKG_COMMIT is set in the environment (the workflow sets a vcpkg whose msys2 runtime exists), the
    # COMMIT line of cesium-native's own CMakeLists.txt is rewritten to it before cmake runs. Unset, nothing
    # changes.
    pinned = os.environ.get("EZVCPKG_COMMIT", "").strip()
    if pinned:
        import re
        cml = os.path.join(repoDirectory, "CMakeLists.txt")
        with open(cml, "r", encoding="utf-8") as fh:
            text = fh.read()
        new_text, count = re.subn(r"(ezvcpkg_fetch\(\s*COMMIT\s+)[0-9a-f]{40}", lambda m: m.group(1) + pinned, text, count=1)
        if count:
            with open(cml, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(new_text)
            print("EZVCPKG_COMMIT: cesium-native's ezvcpkg_fetch COMMIT rewritten to " + pinned)
        else:
            print("EZVCPKG_COMMIT is set but no ezvcpkg_fetch COMMIT line was found in cesium-native's CMakeLists.txt")
    # 28 Oct 2026, run 40643965153: with vcpkg afc0a2e0 glm is 1.0.2, and cesium-native v0.52.1's
    # CesiumGltf/src/Model.cpp stops on glm/detail/type_quat.inl(253) 'mat4_cast': identifier not found.
    # cesium-native fixed it itself between v0.52.1 and v0.54.0 with one include in Model.cpp
    # (CesiumGS/cesium-native compare v0.52.1...v0.54.0); the same line is added here after the clone.
    model_cpp = os.path.join(repoDirectory, "CesiumGltf", "src", "Model.cpp")
    if os.path.isfile(model_cpp):
        with open(model_cpp, "r", encoding="utf-8") as fh:
            model_text = fh.read()
        needle = "#include <glm/geometric.hpp>\n"
        if "#include <glm/gtc/quaternion.hpp>" not in model_text and needle in model_text:
            model_text = model_text.replace(needle, needle + "#include <glm/gtc/quaternion.hpp> // cesium-native v0.54.0's fix for glm 1.0.2\n", 1)
            with open(model_cpp, "w", encoding="utf-8", newline="\n") as fh:
                fh.write(model_text)
            print("Model.cpp: added #include <glm/gtc/quaternion.hpp> (cesium-native's own fix for glm 1.0.2)")
    # Assume you already have the triplet (for now)
    triplet: str = determine_triplet()
    os.environ["VCPKG_TRIPLET"] = triplet
    # Run Cmake with the /MT flag on
    result = subprocess.run(
        [
            "cmake",
            f"-DCMAKE_BUILD_TYPE={RELEASE_CONFIG}",
            "-DCESIUM_MSVC_STATIC_RUNTIME_ENABLED=ON",
            "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
            "-DGIT_LFS_SKIP_SMUDGE=1",
            "-DVCPKG_TRIPLET=%s" % triplet,
            ".",
        ]
    )

    # We pray this works haha
    if result.returncode != 0:
        errorMsg = "cmake return code: %s" % str(result.returncode)
        print(
            "Error configuring Cesium native, please make sure you have CMake installed and up to date: "
            + errorMsg
        )
        exit(1)
    print("Configuration completed without any errors!")


def determine_triplet():
    if os.name == OS_WIN:
        return "x64-windows-static"
    if sys.platform == PLATFORM_MACOS:
        return "arm64-osx"
    if os.name == OS_LINUX:
        return "x64-linux"


def compile_native(argumentsDict):
    shouldBuildArg = argumentsDict.get("buildCesium", None)
    if shouldBuildArg is None:
        shouldBuildResponse = input(
            "Do you wanna build Cesium Native (Choose yes if it's the first install)? [y/n]"
        )
        shouldBuildArg = shouldBuildResponse.capitalize()[0] == "Y"
    else:
        shouldBuildArg = (
            shouldBuildArg.upper() == "YES" or shouldBuildArg.upper() == "TRUE"
        )

    if not shouldBuildArg:
        return

    print("Building Cesium Native, this might take a few minutes...")
    configure_native(argumentsDict)
    print("Compiling Cesium Native...")

    # TODO: Test if we can just do cmake --build for all platforms
    result = None
    if os.name == OS_WIN:
        result = build_native_win(argumentsDict)
    elif sys.platform == PLATFORM_MACOS:
        result = build_native_macos()
    elif os.name == OS_LINUX:
        result = build_native_linux()
    else:
        print(
            "Compiling for platform %s is not yet supported!" % os.name, file=sys.stderr
        )
    if result is None or result.returncode != 0:
        # Fail here, where the cause is, instead of 30 minutes later at the extension's link step.
        print("Error building Cesium Native: return code %s" % (None if result is None else result.returncode), file=sys.stderr)
        exit(1)
    print("Cleaning definitions on generated files...")
    clean_cesium_definitions()
    print("Finished building Cesium Native!")


def build_native_linux():
    return subprocess.run(["cmake", "--build", "."])


def build_native_macos():
    return subprocess.run(["cmake", "--build", "."])


def build_native_win(argumentsDict):
    buildConfig: str = "RelWithDebInfo" if argumentsDict.get("precision") == "double" else RELEASE_CONFIG
    # 28 Oct 2026: GitHub's windows-latest image (windows-2025-vs2026) makes CMake 4.4 generate for
    # "Visual Studio 18 2026", whose solution file is cesium-native.slnx - there is no cesium-native.sln,
    # so MSBuild answered "MSB1009: Project file does not exist", cesium-native never compiled, and the
    # extension's link failed later with LNK1181 Cesium3DTilesSelection.lib (run 40639946574, both jobs).
    # cmake --build drives whatever generator wrote the build files, .sln or .slnx, and --config picks the
    # configuration on a multi-config generator the same way /property:Configuration did.
    return subprocess.run(["cmake", "--build", ".", "--config", buildConfig, "--parallel"])


def clean_cesium_definitions():
    """
    This function modifies some of Cesium's header files to clean up
    definitions that conflict with the engine's
    """
    # Get the conflicting file (Material.h in our case)
    print("Cleaning native definitions")

    conflictFilePath: str = "%s/%s" % (
        CESIUM_NATIVE_DIR_EXT,
        "/CesiumGltf/generated/include/CesiumGltf",
    )
    conflictFilePath = scons_to_abs_path(conflictFilePath) + "/Material.h"
    # Load the file into memory

    # Read in the file
    fileData: str = ""
    with open(conflictFilePath, "r") as file:
        fileData = file.read()

    # Replace the target string
    fileData = fileData.replace("#pragma once", "#pragma once\n#undef OPAQUE")

    # Write the file out again
    with open(conflictFilePath, "w") as file:
        file.write(fileData)
    print("Finished cleaning native definitions")


def install_additional_libs():
    print("Installing additional libraries")
    vcpkgPath = find_ezvcpkg_path()
    execExtension = ".exe" if os.name == OS_WIN else ""
    executable = "%s/%s" % (vcpkgPath, "vcpkg" + execExtension)
    subprocess.run([executable, "install", "uriparser:%s" % (determine_triplet())])
    subprocess.run([executable, "install", "ada-url:%s" % (determine_triplet())])
    if os.name == OS_WIN:
        subprocess.run([executable, "install", "curl:%s" % (determine_triplet())])


def find_ms_build() -> str:
    print("Searching for MS Build")
    # Try to search for an msbuild executable in the system
    try:
        testCmd = subprocess.run(
            ["msbuild", "-version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # Yay, we found it... (not gonna happen lol)
        if testCmd.returncode == 0:
            return "msbuild"
    except:
        # More likely we'll need to search for another path
        vsPath = "C:\\Program Files\\Microsoft Visual Studio"
        found, path = find_in_dir_recursive(vsPath, "*MSBuild.exe")

        if found:
            # Access the next latest directory (latest VS version)
            return path

        # Try with a .NET path
        print(".NET path is not yet supported!", sys.stderr)
        return ""


def find_in_dir_recursive(path: str, pattern: str) -> (bool, str):
    """
    Use only when there might be a few directories left to search
    as this function is recursive
    """

    if not os.path.exists(path):
        return False, ""

    foundFiles: list[str] = os.listdir(path)

    if len(foundFiles) == 0:
        return False, ""

    for root, dirnames, filenames in os.walk(path):
        for filename in fnmatch.filter(filenames, pattern):
            return True, os.path.join(root, filename)

    return False, ""


def find_ezvcpkg_path() -> str:
    global ezvcpkgFoundPath
    if ezvcpkgFoundPath != "":
        return ezvcpkgFoundPath
    # Search the home directory
    assumedPath = "%s.ezvcpkg" % (os.path.abspath(os.sep))
    print(f"Searching vcpkg at: {assumedPath}")
    if not os.path.exists(assumedPath):
        from pathlib import Path

        assumedPath = (Path.home() / ".ezvcpkg").as_posix()
        print(f"Searching vcpkg at: {assumedPath}")
        if not os.path.exists(assumedPath):
            print(
                "EZVCPKG not found, please make sure that CesiumNative was compiled and configured properly!"
            )
            return ""
        # Assume it is in /home (C:/Users/currUser)
    # Then find the latest version (use the last created folder)
    subDirs = [x for x in next(os.walk(assumedPath))[1]]
    subDirs.sort(
        reverse=True, key=lambda x: os.stat("%s/%s" % (assumedPath, x)).st_ctime
    )
    latestDir = subDirs[0]
    ezvcpkgFoundPath = "%s/%s" % (assumedPath, latestDir)
    print(f"Found ezvcpkg at {ezvcpkgFoundPath}")
    return ezvcpkgFoundPath


def clone_engine_repo_if_needed():
    pass


def scons_to_abs_path(path: str) -> str:
    return Dir(path).get_abspath()


def find_ezvcpkg_include_path() -> str:
    return f"{find_ezvcpkg_path()}/installed/{determine_triplet()}/include"


def find_ezvcpkg_lib_path() -> str:
    return f"{find_ezvcpkg_path()}/installed/{determine_triplet()}/lib"


def get_root_dir() -> str:
    return currentRootDir


def get_root_dir_native() -> str:
    return scons_to_abs_path(currentRootDir + "/native")
