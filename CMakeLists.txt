cmake_minimum_required(VERSION 3.24)
project(LASOComputer VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "LASO-Computer is a native Windows application.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(laso_core
  src/config.cpp
  src/protocol.cpp
  src/plugin.cpp
  src/audit.cpp
  src/windows_platform.cpp
  src/playwright.cpp
)
target_include_directories(laso_core PUBLIC include third_party/nlohmann_json/include)
target_compile_definitions(laso_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(laso_core PRIVATE /W4 /permissive- /EHsc /utf-8)
target_link_libraries(laso_core PUBLIC User32 Gdi32 Shell32 Ole32 Windowscodecs Advapi32 Crypt32 Uiautomationcore Userenv Winhttp Ws2_32 Iphlpapi)

add_executable(laso-computer src/main.cpp)
target_link_libraries(laso-computer PRIVATE laso_core)
target_compile_options(laso-computer PRIVATE /W4 /permissive- /EHsc /utf-8)

include(CTest)
if(BUILD_TESTING)
  add_executable(laso-tests tests/tests.cpp)
  target_link_libraries(laso-tests PRIVATE laso_core)
  target_compile_options(laso-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  add_test(NAME laso-tests COMMAND laso-tests)
  add_executable(laso-desktop-tests tests/desktop_tests.cpp)
  target_link_libraries(laso-desktop-tests PRIVATE laso_core Ole32 User32)
  target_compile_options(laso-desktop-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
