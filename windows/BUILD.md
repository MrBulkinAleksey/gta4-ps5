# Building GTA IV for Windows

Work in progress: the build makes the desktop app (`LibertyRecomp.exe`) from
the same ReXGlue consumer as the PS5, Android and macOS ports. The PS5 hub,
trophies and the PS5 display code are not part of it yet.

GitHub Actions builds it on every push (`.github/workflows/windows.yml`) and
uploads the app as the `LibertyRecomp-windows-x64` artifact.

## What you need

- Visual Studio 2022 (or its Build Tools) with the Windows SDK
- LLVM/clang 19 or newer (`clang`, `clang++`, `lld`), CMake 3.29+, Ninja, Python 3.10+, git
- The [Vulkan SDK](https://vulkan.lunarg.com/) (glslangValidator and spirv-val)
- DXC 1.9 (1.8 crashes on the Bink video shaders): `dxc_2025_07_14.zip` from
  DirectXShaderCompiler v1.8.2505.1

No game files are needed to build: the recompiled code is already in the repo.

## 1. Source

From Git Bash:

```
git clone https://github.com/MrBulkinAleksey/gta4-ps5
cd gta4-ps5
git -c submodule.recurse=false submodule update --init --depth 1 --jobs 8 \
  $(grep "path = glue/rexglue-sdk-main/thirdparty" .gitmodules | awk '{print $3}') tools/XenosRecomp
git -C tools/XenosRecomp submodule update --init --depth 1 thirdparty/smol-v thirdparty/zstd thirdparty/xxHash
git -C glue/rexglue-sdk-main/thirdparty/libmspack apply --ignore-space-change \
  "$PWD/cmake/dependency-patches/libmspack-xmem-block-padding.patch"
```

## 2. Build

From a *x64 Native Tools Command Prompt for VS 2022* (or PowerShell after
`vcvars64.bat`), with `C:\dxc` holding the unpacked DXC:

```
cmake -S windows -B out/windows -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_LINKER_TYPE=LLD ^
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS=-march=x86-64-v3 ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DREXGLUE_ENABLE_TRACY=OFF ^
  -DGTA4_NATIVE_DXC_LIBRARY_PATH=C:/dxc/bin/x64
cmake --build out/windows --target LibertyRecomp
```

The app ends up in `out/windows/LibertyRecomp`: the exe, its DLLs and
`Resources/`. `-march=x86-64-v3` (AVX2, as upstream ReXGlue builds) needs a
Haswell / Zen 1 or newer CPU.
