cmake_minimum_required(VERSION 3.16)
project(fs-updater-service VERSION 2.2.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

option(BUILD_TESTING "Build unit tests" OFF)
option(ENABLE_SANITIZERS "Enable ASan and UBSan (native test builds)" OFF)
option(FSUP_WERROR "Treat compiler warnings as errors (-Werror)" ON)

# Global so the test binaries (and fetched gtest) share the sanitizer runtime.
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)

# Allow caller to provide libsystemd paths directly (for native test builds where
# libsystemd-dev is not installed but headers/lib are available from the vendor dir).
# Pass -DLIBSYSTEMD_INCLUDE=<inc_dir> -DLIBSYSTEMD_LIB=<lib_dir> to use this path.
if(DEFINED LIBSYSTEMD_INCLUDE AND DEFINED LIBSYSTEMD_LIB)
    set(LIBSYSTEMD_INCLUDE_DIRS "${LIBSYSTEMD_INCLUDE}")
    set(LIBSYSTEMD_LIBRARIES    "systemd")
    set(LIBSYSTEMD_LIBRARY_DIRS "${LIBSYSTEMD_LIB}")
    set(LIBSYSTEMD_CFLAGS_OTHER "")
else()
    pkg_check_modules(LIBSYSTEMD REQUIRED libsystemd)
endif()


add_executable(fs-updater-service
    src/main.cpp
    src/service.cpp
)

target_include_directories(fs-updater-service PRIVATE src)
# Third-party headers as SYSTEM so -Wpedantic/-Werror never fires on them
# (the vendored libsystemd headers use compound literals + designated inits).
target_include_directories(fs-updater-service SYSTEM PRIVATE ${LIBSYSTEMD_INCLUDE_DIRS})

if(DEFINED LIBSYSTEMD_LIBRARY_DIRS)
    target_link_directories(fs-updater-service PRIVATE ${LIBSYSTEMD_LIBRARY_DIRS})
endif()

target_link_libraries(fs-updater-service PRIVATE
    ${LIBSYSTEMD_LIBRARIES}
)

target_compile_options(fs-updater-service PRIVATE
    ${LIBSYSTEMD_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Wpedantic
    # Gated so a toolchain warning bump can be unblocked with -DFSUP_WERROR=OFF.
    $<$<BOOL:${FSUP_WERROR}>:-Werror>
)

if(DEFINED FUS_LIB_DIR)
    pkg_check_modules(JSONCPP  REQUIRED jsoncpp)
    pkg_check_modules(LIBARCHIVE REQUIRED libarchive)
    pkg_check_modules(BOTAN2 botan-2)
    target_include_directories(fs-updater-service SYSTEM PRIVATE
        "${FUS_LIB_DIR}/include"
        ${JSONCPP_INCLUDE_DIRS}
        ${LIBARCHIVE_INCLUDE_DIRS}
        ${BOTAN2_INCLUDE_DIRS}
    )
    target_link_directories(fs-updater-service PRIVATE "${FUS_LIB_DIR}/lib")
    target_link_libraries(fs-updater-service PRIVATE
        fs_updater
        ${JSONCPP_LIBRARIES}
        ${LIBARCHIVE_LIBRARIES}
        ${BOTAN2_LIBRARIES}
        ubootenv
        z
    )
    target_sources(fs-updater-service PRIVATE src/real_updater.cpp)
    target_compile_definitions(fs-updater-service PRIVATE HAVE_FUS_LIB)
endif()

install(TARGETS fs-updater-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR}
)

install(FILES
    data/fs-updater.service
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
)

install(FILES
    data/de.fsembedded.fsupdate1.service
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/system-services
)

install(FILES
    data/de.fsembedded.fsupdate1.conf
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/dbus-1/system.d
)

install(FILES
    data/de.fsembedded.fsupdate1.policy
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions
)

install(FILES
    data/50-fsupdate.rules
    DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/polkit-1/rules.d
)

install(FILES
    dbus/de.fsembedded.fsupdate1.xml
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/dbus-1/interfaces
)

if(BUILD_TESTING)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
            URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()

    enable_testing()

    add_executable(fs-updater-service-tests
        tests/main_test.cpp
        tests/service_test.cpp
        src/service.cpp
    )

    target_include_directories(fs-updater-service-tests PRIVATE src)
    target_include_directories(fs-updater-service-tests SYSTEM PRIVATE
        ${LIBSYSTEMD_INCLUDE_DIRS}
    )

    target_compile_options(fs-updater-service-tests PRIVATE
        ${LIBSYSTEMD_CFLAGS_OTHER}
    )

    if(DEFINED LIBSYSTEMD_LIBRARY_DIRS)
        target_link_directories(fs-updater-service-tests PRIVATE ${LIBSYSTEMD_LIBRARY_DIRS})
    endif()

    target_link_libraries(fs-updater-service-tests PRIVATE
        GTest::gtest_main
        ${LIBSYSTEMD_LIBRARIES}
    )

    if(DEFINED FUS_LIB_DIR)
        target_include_directories(fs-updater-service-tests PRIVATE "${FUS_LIB_DIR}/include")
        target_link_directories(fs-updater-service-tests PRIVATE "${FUS_LIB_DIR}/lib")
        target_link_libraries(fs-updater-service-tests PRIVATE fs_updater)
    endif()

    add_test(NAME fs-updater-service-tests COMMAND fs-updater-service-tests)
    # Disable the logind sleep-inhibit during unit tests: there is no
    # logind on the test bus, the failure path through sd_bus_call_method
    # destabilises the connection enough to fault during process exit
    # (tests themselves all pass), and the warning spams CI output.
    # Tests that need to exercise the knob explicitly setenv() it back.
    set_tests_properties(fs-updater-service-tests PROPERTIES
        ENVIRONMENT "FSUPDATE_INHIBIT_SLEEP=0")

    # The published interface is documentation nothing parses at runtime, so a
    # method can be exported for years without ever appearing in it. Compare
    # the two directly rather than trusting review to notice.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME interface-contract
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_interface_contract.py
                ${CMAKE_CURRENT_SOURCE_DIR}/dbus/de.fsembedded.fsupdate1.xml
                ${CMAKE_CURRENT_SOURCE_DIR}/src/service.cpp)
    else()
        message(WARNING "python3 not found: the interface-contract check is skipped")
    endif()
endif()
