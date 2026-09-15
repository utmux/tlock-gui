cmake_minimum_required(VERSION 3.21)

project(TLockGUI
    VERSION 1.1.1
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.8.3 EXACT REQUIRED COMPONENTS
    Core
    Gui
    Widgets
)

qt_standard_project_setup()

qt_add_executable(TLockGUI WIN32
    src/main.cpp
    src/AppTranslator.h
    src/AppTranslator.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/FileTask.h
    src/TleRunner.h
    src/TleRunner.cpp
    src/TaskQueue.h
    src/TaskQueue.cpp
    src/HashWorker.h
    src/HashWorker.cpp
    src/PowerManager.h
    src/PowerManager.cpp
    src/Utils.h
    src/Utils.cpp
)

target_link_libraries(TLockGUI PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

if(MINGW)
    target_compile_options(TLockGUI PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(TLockGUI PROPERTIES
    WIN32_EXECUTABLE TRUE
    OUTPUT_NAME "TLockGUI"
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(FakeTle
        tests/FakeTle.cpp
    )
    target_link_libraries(FakeTle PRIVATE Qt6::Core)

    qt_add_executable(TLockGUITests
        tests/TestMain.cpp
        src/AppTranslator.h
        src/AppTranslator.cpp
        src/FileTask.h
        src/TleRunner.h
        src/TleRunner.cpp
        src/TaskQueue.h
        src/TaskQueue.cpp
        src/PowerManager.h
        src/PowerManager.cpp
        src/Utils.h
        src/Utils.cpp
    )
    target_include_directories(TLockGUITests PRIVATE src)
    target_link_libraries(TLockGUITests PRIVATE Qt6::Core)
    if(MINGW)
        target_compile_options(TLockGUITests PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_test(NAME TLockGUITests
        COMMAND TLockGUITests $<TARGET_FILE:FakeTle>
    )

    qt_add_executable(OfficialTleIntegration
        tests/OfficialTleIntegration.cpp
        src/FileTask.h
        src/TleRunner.h
        src/TleRunner.cpp
        src/TaskQueue.h
        src/TaskQueue.cpp
        src/Utils.h
        src/Utils.cpp
    )
    target_include_directories(OfficialTleIntegration PRIVATE src)
    target_link_libraries(OfficialTleIntegration PRIVATE Qt6::Core)
    if(MINGW)
        target_compile_options(OfficialTleIntegration PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    qt_add_executable(TLockGUISmoke
        tests/GuiSmoke.cpp
        src/AppTranslator.h
        src/AppTranslator.cpp
        src/MainWindow.h
        src/MainWindow.cpp
        src/FileTask.h
        src/TleRunner.h
        src/TleRunner.cpp
        src/TaskQueue.h
        src/TaskQueue.cpp
        src/HashWorker.h
        src/HashWorker.cpp
        src/PowerManager.h
        src/PowerManager.cpp
        src/Utils.h
        src/Utils.cpp
    )
    target_include_directories(TLockGUISmoke PRIVATE src)
    target_link_libraries(TLockGUISmoke PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    if(MINGW)
        target_compile_options(TLockGUISmoke PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME TLockGUISmoke COMMAND TLockGUISmoke)
    set_tests_properties(TLockGUISmoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 15
    )
    get_filename_component(TLOCKGUI_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set_tests_properties(TLockGUITests TLockGUISmoke PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;PATH=path_list_prepend:${TLOCKGUI_COMPILER_BIN_DIR}"
    )
endif()
