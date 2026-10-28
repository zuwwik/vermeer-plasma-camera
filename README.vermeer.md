# Plasma Camera for Vermeer

Based on KDE Plasma Camera v2.1.1, with the working Poco F6 Pro / Redmi K70
capture and orientation changes. The original upstream history and licenses
are retained; upstream usage is described in README.md.

The live preview keeps the native libcamera AE/AF/colour pipeline. Photos use
the full RAW array and background still processing, saved as JPEG quality 100.
Front/rear rotation and mirroring follow the device orientation; the viewfinder
aspect follows the rotated frame. This checkpoint does not enable 50 MP,
PDAF, OIS, HDR or Qualcomm ICP.

Build against the revision in `libcamera.lock`, including its installed
`libcamera/still_image.h` development header. Required dependencies are Qt 6
Quick, Multimedia, Concurrent and Sensors, KDE Frameworks/Kirigami, ECM,
Exiv2, CMake, Ninja and pkg-config. Then build normally:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cmake --install build
```

`prebuilt/aarch64-linux-musl/plasma-camera` is the exact deployed application,
for the phone's musl rootfs. Install the matched `vermeer-camera` runtime first;
its installer selects the libcamera library search path. Close camera clients,
verify `sha256sum -c SHA256SUMS`, then install this executable at
`/usr/local/bin/plasma-camera`. A rebuilt native binary can be installed there
instead. The distribution supplies the normal Plasma Camera desktop entry and
Qt/Kirigami runtime dependencies.

The optional `-DVERMEER_CAMERA_TESTS=ON` target exercises RAW stills and preview
reopens; it is not part of the default installed application.
