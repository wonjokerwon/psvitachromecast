cmake_minimum_required(VERSION 3.13)

# Builds the PS Vita app (VPK) when configured with the vitasdk toolchain,
# otherwise a desktop build of the receiver plus tests:
#   cmake -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
#   cmake -B build && cmake --build build && ctest --test-dir build

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK} AND NOT VITACAST_HOST)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(vitacast C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CORE_SOURCES
  src/core/castmsg.c
  src/core/config.c
  src/core/http.c
  src/core/json.c
  src/core/log.c
  src/core/mdns.c
  src/core/net.c
  src/core/receiver.c
  src/core/sb.c
  src/core/tls.c
)

if(VITA)
  include("${VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "VitaCast")
  set(VITA_TITLEID  "VCST00001")
  set(VITA_VERSION  "01.00")

  add_compile_options(-O2 -Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE)
  # vita-elf-create needs the relocations kept in the final ELF, and room
  # after the text segment for the import tables (see sce_gap.ld).
  add_link_options(-Wl,-q -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/src/vita/sce_gap.ld)

  add_executable(vitacast
    ${CORE_SOURCES}
    src/vita/main.c
    src/vita/platform_vita.c
    src/vita/player_vita.c
    src/vita/ui.c
  )

  target_link_libraries(vitacast
    vita2d
    mbedtls
    mbedx509
    mbedcrypto
    png
    jpeg
    z
    m
    pthread
    SceAvPlayer_stub
    SceAudio_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceCommonDialog_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceNet_stub
    SceNetCtl_stub
    ScePgf_stub
    ScePower_stub
    SceSysmodule_stub
    SceTouch_stub
  )

  vita_create_self(eboot.bin vitacast)
  vita_create_vpk(vitacast.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  )
else()
  find_package(Threads REQUIRED)
  find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h REQUIRED)
  find_library(MBEDTLS_LIB mbedtls REQUIRED)
  find_library(MBEDX509_LIB mbedx509 REQUIRED)
  find_library(MBEDCRYPTO_LIB mbedcrypto REQUIRED)

  add_compile_options(-Wall -Wextra -Wno-unused-parameter -D_GNU_SOURCE)
  option(VITACAST_SANITIZE "Build with address/undefined sanitizers" OFF)
  option(VITACAST_TSAN "Build with the thread sanitizer" OFF)
  if(VITACAST_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(VITACAST_TSAN)
    add_compile_options(-fsanitize=thread -g)
    add_link_options(-fsanitize=thread)
  endif()

  add_library(vitacast_core STATIC ${CORE_SOURCES})
  target_include_directories(vitacast_core PUBLIC ${MBEDTLS_INCLUDE_DIR})
  target_link_libraries(vitacast_core PUBLIC
    ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB} Threads::Threads m)

  add_executable(vitacast-host
    src/host/main.c
    src/host/platform_host.c
    src/host/player_sim.c
  )
  target_link_libraries(vitacast-host vitacast_core)

  enable_testing()
  add_executable(test_core
    tests/test_core.c
    src/host/platform_host.c
    src/host/player_sim.c
  )
  target_link_libraries(test_core vitacast_core)
  add_test(NAME core COMMAND test_core)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME e2e
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e_test.py
              $<TARGET_FILE:vitacast-host>)
  endif()
endif()
