cmake_minimum_required(VERSION 3.24)

# ---- 버전 단일 출처 ----
# 여기만 고치면 exe 버전 리소스, 앱의 '정보' 탭, 설치 프로그램 파일명까지 함께 따라온다
# (설치 프로그램은 scripts/build.ps1이 ISCC에 /DMyAppVersion으로 넘긴다).
project(SuperSticker VERSION 1.5.9 LANGUAGES CXX)
set(SS_RELEASE_DATE "2026-09-02")  # 릴리스 날짜 — 버전과 함께 올린다

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WebView2LoaderStatic.lib은 정적 CRT(/MT)를 요구한다
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

FetchContent_Declare(webview2
  URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4129.50/microsoft.web.webview2.1.0.4129.50.nupkg
  DOWNLOAD_NAME webview2.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(wil
  URL https://api.nuget.org/v3-flatcontainer/microsoft.windows.implementationlibrary/1.0.260126.7/microsoft.windows.implementationlibrary.1.0.260126.7.nupkg
  DOWNLOAD_NAME wil.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
  DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(webview2 wil nlohmann_json)

add_executable(SuperSticker WIN32
  src/main.cpp
  src/App.cpp
  src/WebViewHost.cpp
  src/StickerWindow.cpp
  src/GroupWindow.cpp
  src/ManagerWindow.cpp
  src/TrayIcon.cpp
  src/Bridge.cpp
  src/Store.cpp
  src/AiClient.cpp
  src/LocalAi.cpp
  src/Theme.cpp
  src/I18n.cpp
  src/Autostart.cpp
  src/Utils.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc
  src/app.manifest
)

target_compile_definitions(SuperSticker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  $<$<CONFIG:Debug>:SS_DEBUG=1>
)
target_compile_options(SuperSticker PRIVATE /utf-8 /W4 /EHsc $<$<CONFIG:Release>:/O2>)

# 버전 정보를 헤더와 리소스로 흘려보낸다 (아이콘 경로는 생성 위치가 달라지므로 절대 경로)
set(SS_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/icons/app.ico")
configure_file(src/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h @ONLY)
configure_file(src/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc @ONLY)

target_include_directories(SuperSticker PRIVATE
  ${CMAKE_CURRENT_BINARY_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${webview2_SOURCE_DIR}/build/native/include
  ${wil_SOURCE_DIR}/include
  ${nlohmann_json_SOURCE_DIR}
)

target_link_libraries(SuperSticker PRIVATE
  ${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib
  winhttp ws2_32 shlwapi comctl32 dwmapi shell32 ole32 user32 gdi32 gdiplus advapi32 shcore crypt32
)

# ui 폴더를 실행 파일 옆으로 복사.
# POST_BUILD로 달아 두면 C++이 안 바뀐 빌드(= UI만 고친 빌드)에서는 타깃이 최신이라 복사가
# 통째로 건너뛰어져 build/release/ui가 낡은 채로 남는다(실측 — 바뀐 UI가 안 보인다).
# 항상 도는 별도 타깃으로 둔다.
add_custom_target(copy_ui ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/ui $<TARGET_FILE_DIR:SuperSticker>/ui
  COMMENT "ui 폴더 복사"
)
add_dependencies(copy_ui SuperSticker)

# 스트리밍 파서 확인용 콘솔 테스트 (기본 빌드에는 들어가지 않는다)
#   cmake --build --preset release --target ai_stream_test
add_executable(ai_stream_test EXCLUDE_FROM_ALL
  tests/ai_stream_test.cpp
  src/AiClient.cpp
  src/Utils.cpp
)
target_compile_definitions(ai_stream_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(ai_stream_test PRIVATE /utf-8 /W4 /EHsc)
target_include_directories(ai_stream_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${nlohmann_json_SOURCE_DIR}
)
target_link_libraries(ai_stream_test PRIVATE winhttp shlwapi ole32 shell32 advapi32 crypt32 shcore user32)
