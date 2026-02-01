# tesuto-launcher

### Dependencies
- Qt 6 (Core, Network)
- Java 17+ (OpenJDK), `unzip` (for natives)

### Build
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

### Notes
- Settings are stored in QSettings under `Tesuto/TesutoLauncher`.
- Optionally, you can enable storing the refresh token in the keychain: compile with `-DUSE_QKEYCHAIN=ON`
(requires QtKeychain / qt6keychain).
- External `unzip` and `tar` are currently used to unpack the natives/Temurin JRE. For a future port to Windows, it is better to replace it with built-in unpacking (libarchive / minizip / QuaZip).
