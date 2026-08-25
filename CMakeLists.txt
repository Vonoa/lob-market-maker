cmake_minimum_required(VERSION 3.16)
project(LOB_Sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# core/, strategies/, and simulation/ are added as include directories (not
# folded into a library) so every file can keep bare #include "OrderBook.h"
# style includes regardless of which folder it lives in - no relative ../
# paths to update the next time something moves.
set(LOBSIM_INCLUDE_DIRS
    ${CMAKE_SOURCE_DIR}/core
    ${CMAKE_SOURCE_DIR}/strategies
    ${CMAKE_SOURCE_DIR}/simulation
)

add_executable(lobsim
    main.cpp
    core/Order.cpp
    core/OrderBook.cpp
    core/MatchingEngine.cpp
    core/MarketMaker.cpp
    core/FairValueModel.cpp
    core/Logging.cpp
    strategies/DefaultStrategy.cpp
    strategies/FixedSpreadStrategy.cpp
    simulation/RandomTrader.cpp
    simulation/HistoricalDataReplay.cpp
    simulation/SimulationEngine.cpp
    simulation/Recorder.cpp
)
target_include_directories(lobsim PRIVATE ${LOBSIM_INCLUDE_DIRS})

# Historical data CSVs (multi-hundred-MB each) live in data/, not copied
# anywhere - LOBSIM_DATA_DIR bakes in that folder's absolute path at compile
# time so it resolves regardless of the process's working directory, without
# duplicating gigabytes of data into build/ on every configure.
target_compile_definitions(lobsim PRIVATE LOBSIM_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

# --- Tests ---------------------------------------------------------------
include(FetchContent)
# doctest's own CMakeLists.txt declares an old cmake_minimum_required() that
# newer CMake rejects outright. This tells CMake to apply pre-3.5 policy
# behaviour when configuring doctest specifically, without affecting our own
# project's policies.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

enable_testing()

add_executable(lobsim_tests
    tests/test_main.cpp
    tests/test_orderbook.cpp
    tests/test_matching_engine.cpp
    tests/test_cost_basis.cpp
    tests/test_event_queue.cpp
    core/Order.cpp
    core/OrderBook.cpp
    core/MatchingEngine.cpp
    core/MarketMaker.cpp
    core/FairValueModel.cpp
    core/Logging.cpp
    strategies/DefaultStrategy.cpp
    strategies/FixedSpreadStrategy.cpp
    simulation/RandomTrader.cpp
    simulation/HistoricalDataReplay.cpp
    simulation/SimulationEngine.cpp
    simulation/Recorder.cpp
)
target_include_directories(lobsim_tests PRIVATE ${LOBSIM_INCLUDE_DIRS})
target_link_libraries(lobsim_tests PRIVATE doctest::doctest)
# test_event_queue.cpp exercises SimulationEngine's Historical path against a
# real (small slice of a) data file - same mechanism main.cpp uses.
target_compile_definitions(lobsim_tests PRIVATE LOBSIM_DATA_DIR="${CMAKE_SOURCE_DIR}/data")

include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
doctest_discover_tests(lobsim_tests)

# --- GUI -------------------------------------------------------------------
# Off by default so a plain `cmake --build build` (CLI-only) stays fast and
# doesn't need OpenGL. Turn on with -DBUILD_GUI=ON.
option(BUILD_GUI "Build the ImGui/ImPlot GUI (lobsim_gui)" OFF)

if(BUILD_GUI)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    # We only need GLFW as a library, not its own examples/tests/docs build.
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)

    # Dear ImGui and ImPlot don't ship their own CMakeLists.txt for the core
    # library - FetchContent only gets us the source, so we wrap it in our
    # own add_library() below, same as anyone integrating ImGui via CMake has to.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
    )
    FetchContent_MakeAvailable(imgui)

    FetchContent_Declare(implot
        GIT_REPOSITORY https://github.com/epezent/implot.git
        GIT_TAG v0.16
    )
    FetchContent_MakeAvailable(implot)

    find_package(OpenGL REQUIRED)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

    add_library(implot STATIC
        ${implot_SOURCE_DIR}/implot.cpp
        ${implot_SOURCE_DIR}/implot_items.cpp
        ${implot_SOURCE_DIR}/implot_demo.cpp
    )
    target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
    target_link_libraries(implot PUBLIC imgui)

    add_executable(lobsim_gui
        gui/main_gui.cpp
        core/Order.cpp
        core/OrderBook.cpp
        core/MatchingEngine.cpp
        core/MarketMaker.cpp
        core/FairValueModel.cpp
        core/Logging.cpp
        strategies/DefaultStrategy.cpp
        strategies/FixedSpreadStrategy.cpp
        simulation/RandomTrader.cpp
        simulation/HistoricalDataReplay.cpp
        simulation/SimulationEngine.cpp
        simulation/Recorder.cpp
    )
    target_include_directories(lobsim_gui PRIVATE ${LOBSIM_INCLUDE_DIRS})
    target_link_libraries(lobsim_gui PRIVATE imgui implot)
    target_compile_definitions(lobsim_gui PRIVATE LOBSIM_DATA_DIR="${CMAKE_SOURCE_DIR}/data")
endif()
