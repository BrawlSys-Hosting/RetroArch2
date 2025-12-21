![](doc/images/ggpo_header.png)

&nbsp; _[![Appveyor build status](https://img.shields.io/appveyor/ci/pond3r/ggpo/master.svg?logo=appveyor)](https://ci.appveyor.com/project/pond3r/ggpo/branch/master)_

(日本語ドキュメントは[こちら](README.ja.md))

## What's GGPO?

Traditional techniques account for network transmission time by adding delay to a players input, resulting in a sluggish, laggy game-feel.  Rollback networking uses input prediction and speculative execution to send player inputs to the game immediately, providing the illusion of a zero-latency network.  Using rollback, the same timings, reactions, visual and audio queues, and muscle memory your players build up playing offline will translate directly online.  The GGPO networking SDK is designed to make incorporating rollback networking into new and existing games as easy as possible.  

For more information about the history of GGPO, check out http://ggpo.net/

This repository contains the code, documentation, and sample applications for the SDK.

## Building

Building GGPO is available on Windows, Linux, and macOS. The Vector War sample remains Windows-only.

## Recent Changes

- Added Linux and macOS support for the core GGPO SDK (Vector War remains Windows-only).
- Replaced the internal zlib headers with always-on LZ4 compression for saved rollback states.
- Modernized the build baseline to C++11.
- Added a small `GGPOSyncPerf` harness to measure rollback save/load throughput and compression ratios (`-DGGPO_BUILD_PERF=ON`).
- Added configurable LZ4 acceleration for rollback state compression (`ggpo.sync.lz4_accel`).

### Windows

Windows builds requires both [Visual Studio 2019](https://visualstudio.microsoft.com/downloads/) and [CMake](https://cmake.org/download/).  Make sure you've installed both before starting.  Make sure to add CMake to your path when installing.

- Run the `build_windows.cmd` in the root directory of the SDK to generate the Visual Studio 2019 solution files.   
- Open `build/GGPO.sln` solution for Visual Studio 2019 to compile.

You can also run the `cmake-gui` tool if you prefer. 

### Linux

Install a C++ compiler and [CMake](https://cmake.org/download/).

- Configure: `cmake -S . -B build -DGGPO_BUILD_VECTORWAR=OFF`
- Build: `cmake --build build`

### macOS (x86_64 and Apple Silicon)

Install Xcode command line tools and [CMake](https://cmake.org/download/).

- Configure: `cmake -S . -B build -DGGPO_BUILD_VECTORWAR=OFF`
- Build: `cmake --build build`

## Sample Application

The Vector War application in the source directory contains a simple application which uses GGPO to synchronize the two clients.  The command line arguments are:

```
vectorwar.exe  <localport>  <num players> ('local' | <remote ip>:<remote port>) for each player
```

See the .cmd files in the bin directory for examples on how to start 2, 3, and 4 player games.

## Perf Harness

Build the perf target and run it from your build output directory:

```
GGPOSyncPerf --state-kb=256 --frames=2000 --loads=2000 --lz4-accel=2
```

## Licensing

GGPO is available under The MIT License. This means GGPO is free for commercial and non-commercial use. Attribution is not required, but appreciated. 
