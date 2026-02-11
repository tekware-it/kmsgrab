# kmsgrab

KMS/DRM screenshot tool with PNG/JPEG output and optional scaling.

## Implemented Changes (Summary)

These changes were added in this workspace:

1. Pitch-aware framebuffer copy
   The framebuffer is copied row-by-row using the real pitch to avoid tearing/black lines.
2. FB2 handle and pitch usage
   The code now uses `drmModeGetFB2` to read the correct buffer handle and pitch, and maps `pitch * height` bytes.
3. Debug logging behind a flag
   All debug prints are gated behind `-v`.
4. JPEG output support
   Output can be saved as `.jpg` or `.jpeg` using libjpeg.
5. Output scaling with aspect preservation
   `-width` and `-height` allow scaling; if only one is provided, the other is computed to keep aspect ratio.
6. Bilinear scaling option
   `-bilinear` switches scaling from nearest-neighbor to bilinear.
7. JPEG quality flag
   `--quality` (or `-quality`) lets you set JPEG quality (1 to 100, default 90).
8. Daemon mode with IPC trigger
   `--daemon` starts a Unix socket server; IPC supports only `GRAB`.

## Build Requirements

Dependencies:
- `libdrm`
- `libpng`
- `libjpeg`
- `cmake`, `pkg-config`, and a C compiler

On Debian/Ubuntu/Raspberry Pi OS:

```bash
sudo apt-get install -y build-essential cmake pkg-config libdrm-dev libpng-dev libjpeg-dev
```

## Build

```bash
mkdir -p build
cd build
cmake ..
make -j
```

The executable will be `build/kmsgrab`.

## Usage

Basic PNG:

```bash
sudo ./kmsgrab out.png
```

Basic JPEG:

```bash
sudo ./kmsgrab out.jpg
```

Verbose debug logging:

```bash
sudo ./kmsgrab -v out.png
```

Scale to width (height auto-calculated):

```bash
sudo ./kmsgrab -width 1280 out.png
```

Scale to height (width auto-calculated):

```bash
sudo ./kmsgrab -height 720 out.jpg
```

Bilinear scaling and JPEG quality:

```bash
sudo ./kmsgrab -bilinear -width 1280 --quality 85 out.jpg
```

Daemon mode (fixed output path from CLI):

```bash
sudo ./kmsgrab --daemon --socket /tmp/kmsgrab.sock -bilinear -width 1280 --quality 85 out.jpg
```

Trigger a capture via IPC:

```bash
printf "GRAB\n" | socat - UNIX-CONNECT:/tmp/kmsgrab.sock
```

Expected replies:
- `OK` when capture succeeds
- `ERR ...` on failure or unsupported command

## Notes

- Scaling happens after conversion to RGB24 and applies to both PNG and JPEG.
- Debug output is only shown with `-v`.
- In daemon mode, IPC command does not carry options or output path.
- The output filename/options come from daemon startup arguments, and each `GRAB` overwrites the same file.
