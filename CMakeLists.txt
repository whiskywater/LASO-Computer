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

# The privileged broker has a separate, fixed RPC surface. Keep its protocol
# library independent from the endpoint runtime and expose no generic executor.
add_library(laso_broker_core src/broker_protocol.cpp)
target_include_directories(laso_broker_core PUBLIC include third_party/nlohmann_json/include)
target_compile_definitions(laso_broker_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(laso_broker_core PRIVATE /W4 /permissive- /EHsc /utf-8)

add_executable(LASOComputerBroker src/broker_service.cpp)
target_link_libraries(LASOComputerBroker PRIVATE laso_broker_core Advapi32 Userenv Shell32)
target_compile_options(LASOComputerBroker PRIVATE /W4 /permissive- /EHsc /utf-8)

add_executable(LASOComputerBrokerCtl src/brokerctl.cpp)
target_link_libraries(LASOComputerBrokerCtl PRIVATE laso_broker_core Advapi32)
target_compile_options(LASOComputerBrokerCtl PRIVATE /W4 /permissive- /EHsc /utf-8)

add_executable(laso-computer src/main.cpp)
target_link_libraries(laso-computer PRIVATE laso_core)
target_compile_options(laso-computer PRIVATE /W4 /permissive- /EHsc /utf-8)

include(CTest)
if(BUILD_TESTING)
  add_executable(laso-tests tests/tests.cpp)
  target_link_libraries(laso-tests PRIVATE laso_core)
  target_compile_options(laso-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  add_test(NAME laso-tests COMMAND laso-tests)
  add_executable(laso-broker-tests tests/broker_tests.cpp)
  target_link_libraries(laso-broker-tests PRIVATE laso_broker_core)
  target_compile_options(laso-broker-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  add_test(NAME laso-broker-tests COMMAND laso-broker-tests)
  add_executable(laso-desktop-tests tests/desktop_tests.cpp)
  target_link_libraries(laso-desktop-tests PRIVATE laso_core Ole32 User32)
  target_compile_options(laso-desktop-tests PRIVATE /W4 /permissive- /EHsc /utf-8)
  # Manual Windows experiment; it launches Node under a token with all
  # optional privileges disabled and reports actual file/process access.
  add_executable(laso-restricted-token-probe tests/restricted_token_probe.cpp)
  target_link_libraries(laso-restricted-token-probe PRIVATE Advapi32)
  target_compile_options(laso-restricted-token-probe PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
