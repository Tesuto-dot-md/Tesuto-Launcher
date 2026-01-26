cmake_minimum_required(VERSION 3.20)

project(tesuto-launcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt автоматическая генерация
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt6
find_package(Qt6 6.2 REQUIRED COMPONENTS Core Widgets Network Concurrent LinguistTools)



# Optional QtKeychain (Qt6 or Qt5), enable only when actually found
option(USE_QKEYCHAIN "Use QtKeychain for secure token storage" OFF)
set(KeychainTarget "")
if (USE_QKEYCHAIN)
    find_package(Qt6Keychain QUIET)
    if (Qt6Keychain_FOUND AND TARGET Qt6::Keychain)
        set(KeychainTarget Qt6::Keychain)
    else()
        find_package(Qt5Keychain QUIET)
        if (Qt5Keychain_FOUND AND TARGET Qt5::Keychain)
            set(KeychainTarget Qt5::Keychain)
        else()
            find_package(QtKeychain QUIET)
            if (QtKeychain_FOUND AND (TARGET Qt6::Keychain OR TARGET Qt5::Keychain))
                if (TARGET Qt6::Keychain)
                    set(KeychainTarget Qt6::Keychain)
                else()
                    set(KeychainTarget Qt5::Keychain)
                endif()
            endif()
        endif()
    endif()
    if (KeychainTarget STREQUAL "")
        message(WARNING "QtKeychain not found; building WITHOUT keychain support")
        set(USE_QKEYCHAIN OFF)
    else()
        message(STATUS "Using Keychain target: ${KeychainTarget}")
        add_compile_definitions(USE_QKEYCHAIN=1)
    endif()
endif()

# ==== Источники проекта ====
# Собираем все .cpp/.cxx
file(GLOB_RECURSE SRC_ALL
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.cxx"
)


qt_add_resources(tesuto-launcher i18n_res ${CMAKE_SOURCE_DIR}/src/ui/i18n.qrc)

# Исключаем CLI entry-points (если где-то лежат)
list(FILTER SRC_ALL EXCLUDE REGEX ".*/src/main\\.cpp$")
list(FILTER SRC_ALL EXCLUDE REGEX ".*/src/gui_main\\.cpp$")

# Ресурсы (.qrc), если есть
file(GLOB_RECURSE RES_ALL
    "${CMAKE_SOURCE_DIR}/src/*.qrc"
)
list(APPEND SRC_ALL ${RES_ALL})

# Явно добавляем GUI-точку входа
list(APPEND SRC_ALL "${CMAKE_SOURCE_DIR}/src/ui/main_gui.cpp")

# ==== Исполняемый файл только GUI ====
add_executable(tesuto-launcher ${SRC_ALL})

target_include_directories(tesuto-launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)


target_link_libraries(tesuto-launcher PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
)

target_sources(tesuto-launcher PRIVATE
    src/SettingsMigration.cpp
    src/SettingsMigration.h
    src/JavaUtil.cpp
    src/JavaUtil.h
    src/JvmArgsUtil.cpp
    src/JvmArgsUtil.h
)


# Для Windows корректное имя и иконка (если есть .rc)
if (WIN32)
    set_target_properties(tesuto-launcher PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# Установка
install(TARGETS tesuto-launcher RUNTIME DESTINATION bin)

# Link keychain if available
if (USE_QKEYCHAIN AND NOT KeychainTarget STREQUAL "")
    target_link_libraries(tesuto-launcher PRIVATE ${KeychainTarget})
endif()
