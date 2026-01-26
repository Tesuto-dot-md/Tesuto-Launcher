# tesuto-launcher


### Зависимости
- Qt 6 (Core, Network)
- Java 17+ (OpenJDK), `unzip` (для natives)


### Сборка
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j
```

### Примечания
- Настройки хранятся в QSettings под `Tesuto/TesutoLauncher`.
- Опционально можно включить хранение refresh-токена в keychain: соберите с `-DUSE_QKEYCHAIN=ON`
  (нужен QtKeychain / qt6keychain).
- Для распаковки natives/Temurin JRE сейчас используются внешние `unzip` и `tar`.
  Для будущего порта на Windows лучше заменить на встроенную распаковку (libarchive / minizip / QuaZip).