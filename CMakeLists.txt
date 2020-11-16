cmake_minimum_required(VERSION 3.0.2 FATAL_ERROR)

project(TelegramBotApi VERSION 5.0 LANGUAGES CXX)

add_subdirectory(td EXCLUDE_FROM_ALL)

if (NOT DEFINED CMAKE_MODULE_PATH)
  set(CMAKE_MODULE_PATH "")
endif()
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/td/CMake" "${CMAKE_MODULE_PATH}")

if (NOT DEFINED CMAKE_INSTALL_BINDIR)
  set(CMAKE_INSTALL_BINDIR "bin")
endif()

if (POLICY CMP0054)
  # do not expand quoted arguments
  cmake_policy(SET CMP0054 NEW)
endif()
if (POLICY CMP0060)
  # link libraries by full path
  cmake_policy(SET CMP0060 NEW)
endif()

include(PreventInSourceBuild)
prevent_in_source_build()

set(CMAKE_THREAD_PREFER_PTHREAD ON)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if (THREADS_HAVE_PTHREAD_ARG)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread")
endif()

include(TdSetUpCompiler)
td_set_up_compiler()

if (CLANG OR GCC)
  if (MEMPROF)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag(-no-pie CXX_NO_PIE_FLAG)
    if (CXX_NO_PIE_FLAG)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")
    elseif (APPLE)
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-no_pie")
    endif()
  endif()
endif()

set(TELEGRAM_BOT_API_SOURCE
  telegram-bot-api/telegram-bot-api.cpp

  telegram-bot-api/Client.cpp
  telegram-bot-api/ClientManager.cpp
  telegram-bot-api/HttpConnection.cpp
  telegram-bot-api/HttpStatConnection.cpp
  telegram-bot-api/Query.cpp
  telegram-bot-api/Stats.cpp
  telegram-bot-api/WebhookActor.cpp

  telegram-bot-api/Client.h
  telegram-bot-api/ClientManager.h
  telegram-bot-api/ClientParameters.h
  telegram-bot-api/HttpConnection.h
  telegram-bot-api/HttpServer.h
  telegram-bot-api/HttpStatConnection.h
  telegram-bot-api/Query.h
  telegram-bot-api/Stats.h
  telegram-bot-api/WebhookActor.h
)

add_executable(telegram-bot-api ${TELEGRAM_BOT_API_SOURCE})
target_include_directories(telegram-bot-api PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(telegram-bot-api PRIVATE memprof tdactor tdcore tddb tdnet tdutils)

install(TARGETS telegram-bot-api RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if (MSVC AND VCPKG_TOOLCHAIN)
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/" DESTINATION "${CMAKE_INSTALL_BINDIR}" FILES_MATCHING PATTERN "*.dll" PATTERN "*.pdb")
endif()
