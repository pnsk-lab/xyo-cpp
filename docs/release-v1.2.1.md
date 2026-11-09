# xyo-cpp v1.2.1 — platform build fixes

Patch release for v1.2.0. The performance defaults remain unchanged.

- macOS x86_64/arm64 (Xcode 16.4 libc++): floating-point std::from_chars is not available. Use the existing full Scratch decimal grammar validation followed by C-locale strtod_l on Apple. Short exact-integer parsing remains unchanged.
- Windows (MSYS2): qualify xyo::BlockInput in the CLI to avoid ambiguity with the Win32 BlockInput API.

The v1.2.0 tag remains intact for reproducibility. v1.2.1 supersedes that release.
