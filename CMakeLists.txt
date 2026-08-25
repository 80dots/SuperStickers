cmake_minimum_required(VERSION 3.24)
project(SuperSticker VERSION 1.0.0 LANGUAGES CXX)

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
  src/OllamaClient.cpp
  src/Theme.cpp
  src/I18n.cpp
  src/Autostart.cpp
  src/Utils.cpp
  src/app.rc
  src/app.manifest
)

target_compile_definitions(SuperSticker PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  $<$<CONFIG:Debug>:SS_DEBUG=1>
)
target_compile_options(SuperSticker PRIVATE /utf-8 /W4 /EHsc $<$<CONFIG:Release>:/O2>)

target_include_directories(SuperSticker PRIVATE
  ${webview2_SOURCE_DIR}/build/native/include
  ${wil_SOURCE_DIR}/include
  ${nlohmann_json_SOURCE_DIR}
)

target_link_libraries(SuperSticker PRIVATE
  ${webview2_SOURCE_DIR}/build/native/x64/WebView2LoaderStatic.lib
  winhttp shlwapi comctl32 dwmapi shell32 ole32 user32 gdi32 gdiplus advapi32 shcore crypt32
)

# ui 폴더를 실행 파일 옆으로 복사
add_custom_command(TARGET SuperSticker POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/ui $<TARGET_FILE_DIR:SuperSticker>/ui
)
