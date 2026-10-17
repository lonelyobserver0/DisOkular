cmake_minimum_required(VERSION 3.21)
project(DisOkular VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Pdf PdfWidgets WebEngineWidgets Network Svg)
find_package(md4c REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYNCTEX REQUIRED IMPORTED_TARGET synctex)
qt_standard_project_setup()
include(GNUInstallDirs)

qt_add_executable(disokular
    src/main.cpp
    src/MainWindow.h      src/MainWindow.cpp
    src/DocumentView.h    src/DocumentView.cpp
    src/PdfView.h         src/PdfView.cpp
    src/MarkdownView.h    src/MarkdownView.cpp
    src/MarkdownRenderer.h src/MarkdownRenderer.cpp
    src/OutlinePanel.h    src/OutlinePanel.cpp
    src/SyncTex.h         src/SyncTex.cpp
    src/SingleInstance.h  src/SingleInstance.cpp
    src/History.h         src/History.cpp
    src/WelcomePage.h     src/WelcomePage.cpp
    src/ImageView.h       src/ImageView.cpp
    src/LatexView.h       src/LatexView.cpp
)

# Foglio di stile e script del Markdown, più le librerie di terze parti (offline).
qt_add_resources(disokular "markdown"
    PREFIX "/md"
    BASE resources
    FILES resources/markdown.css resources/markdown.js
)
# Icona della finestra, quando il tema non ne fornisce una.
qt_add_resources(disokular "icons"
    PREFIX "/icons"
    BASE resources
    FILES resources/disokular.svg
)
file(GLOB KATEX_FONTS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} third_party/katex/fonts/*.woff2)
qt_add_resources(disokular "markdown_third_party"
    PREFIX "/md"
    BASE third_party
    FILES
        third_party/highlight/highlight.min.js
        third_party/highlight/latex.min.js
        third_party/highlight/cmake.min.js
        third_party/highlight/github.min.css
        third_party/highlight/github-dark.min.css
        third_party/katex/katex.min.js
        third_party/katex/katex.min.css
        ${KATEX_FONTS}
)

target_link_libraries(disokular PRIVATE
    Qt6::Widgets Qt6::Pdf Qt6::PdfWidgets Qt6::WebEngineWidgets Qt6::Network Qt6::Svg
    md4c::md4c-html
    PkgConfig::SYNCTEX
)
target_compile_options(disokular PRIVATE -Wall -Wextra)

install(TARGETS disokular RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# Launcher, associazioni ai tipi di file e icona (il nome "disokular" li lega tra loro
# e all'app_id della finestra su Wayland: vedi setDesktopFileName in main.cpp).
install(FILES resources/disokular.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/disokular.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
