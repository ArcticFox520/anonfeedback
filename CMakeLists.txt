cmake_minimum_required(VERSION 3.16)

project(anonfeedback
        VERSION 1.0.0
        DESCRIPTION "Anonymous feedback collection service"
        LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

find_package(PkgConfig REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(MHD REQUIRED IMPORTED_TARGET libmicrohttpd)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)
pkg_check_modules(UUID REQUIRED IMPORTED_TARGET uuid)

set(ANONFB_SOURCES
    src/config.c
    src/db.c
    src/handlers.c
    src/http.c
    src/log.c
    src/ratelimit.c
    src/util.c)

add_library(anonfb_core STATIC ${ANONFB_SOURCES})
target_include_directories(anonfb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(anonfb_core
    PUBLIC
        PkgConfig::MHD
        PkgConfig::SQLITE3
        PkgConfig::CJSON
        PkgConfig::UUID
        OpenSSL::Crypto
        Threads::Threads)

set(ANONFB_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wpointer-arith
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wvla
    -Wformat=2
    -Wundef)
if(WARNINGS_AS_ERRORS)
  list(APPEND ANONFB_WARNINGS -Werror)
endif()

target_compile_options(anonfb_core PRIVATE ${ANONFB_WARNINGS})
target_compile_definitions(anonfb_core PUBLIC
    _POSIX_C_SOURCE=200809L
    _DEFAULT_SOURCE)

if(ENABLE_SANITIZERS)
  target_compile_options(anonfb_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(anonfb_core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(anonfeedback src/main.c)
target_link_libraries(anonfeedback PRIVATE anonfb_core)
target_compile_options(anonfeedback PRIVATE ${ANONFB_WARNINGS})
target_compile_definitions(anonfeedback PRIVATE ANONFB_VERSION="${PROJECT_VERSION}")

install(TARGETS anonfeedback RUNTIME DESTINATION bin)
install(DIRECTORY static/ DESTINATION share/anonfeedback/static)
install(DIRECTORY migrations/ DESTINATION share/anonfeedback/migrations)

enable_testing()

foreach(unit_test IN ITEMS test_util test_ratelimit)
  add_executable(${unit_test} tests/${unit_test}.c)
  target_link_libraries(${unit_test} PRIVATE anonfb_core)
  target_compile_options(${unit_test} PRIVATE ${ANONFB_WARNINGS})
  add_test(NAME ${unit_test} COMMAND ${unit_test})
endforeach()

add_executable(test_db tests/test_db.c)
target_link_libraries(test_db PRIVATE anonfb_core)
target_compile_options(test_db PRIVATE ${ANONFB_WARNINGS})
add_test(NAME test_db COMMAND test_db ${CMAKE_CURRENT_SOURCE_DIR}/migrations)

add_test(NAME integration
         COMMAND ${CMAKE_COMMAND} -E env
                 SERVER_BIN=$<TARGET_FILE:anonfeedback>
                 STATIC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/static
                 MIGRATIONS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/migrations
                 bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.sh)
