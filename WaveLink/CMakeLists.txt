cmake_minimum_required(VERSION 3.14)

# Normal builds use the version below. Release scripts may provide
# -DWAVELINK_VERSION_OVERRIDE=x.y.z for a one-off manually versioned build.
set(WAVELINK_DEFAULT_VERSION 0.14)
set(WAVELINK_VERSION_OVERRIDE "" CACHE STRING "Optional WaveLink version override")
if(WAVELINK_VERSION_OVERRIDE)
    project(WaveLink VERSION "${WAVELINK_VERSION_OVERRIDE}" LANGUAGES CXX)
else()
    project(WaveLink VERSION "${WAVELINK_DEFAULT_VERSION}" LANGUAGES CXX)
endif()

# Generate Windows version metadata from the project version above. This keeps
# the application, release scripts and executable properties in sync.
set(WAVELINK_VERSION_PATCH_VALUE "${PROJECT_VERSION_PATCH}")
set(WAVELINK_VERSION_TWEAK_VALUE "${PROJECT_VERSION_TWEAK}")
if(WAVELINK_VERSION_PATCH_VALUE STREQUAL "")
    set(WAVELINK_VERSION_PATCH_VALUE 0)
endif()
if(WAVELINK_VERSION_TWEAK_VALUE STREQUAL "")
    set(WAVELINK_VERSION_TWEAK_VALUE 0)
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app_version.rc"
    @ONLY
)

set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()
# Find Qt 5
find_package(Qt5 COMPONENTS Core Gui Widgets SerialPort Network PrintSupport OpenGL Concurrent REQUIRED)
option(WAVELINK_ENABLE_BLE "Enable the Windows BLE interface" ON)

set(WAVELINK_RESOURCE_FILES resources/resources.qrc)
# Release scripts supply an archive from their own working directory. Normal
# IDE builds retain the optional resources/source.zip behavior. Never copy the
# release archive into the source tree: that would invalidate other builds.
set(WAVELINK_SOURCE_ARCHIVE "" CACHE FILEPATH "Optional external source archive to embed")
if(WAVELINK_SOURCE_ARCHIVE)
    get_filename_component(WAVELINK_SOURCE_ARCHIVE_PATH
        "${WAVELINK_SOURCE_ARCHIVE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
    if(NOT EXISTS "${WAVELINK_SOURCE_ARCHIVE_PATH}" OR
       IS_DIRECTORY "${WAVELINK_SOURCE_ARCHIVE_PATH}")
        message(FATAL_ERROR "Source archive does not exist: ${WAVELINK_SOURCE_ARCHIVE_PATH}")
    endif()
else()
    set(WAVELINK_SOURCE_ARCHIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/source.zip")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WAVELINK_SOURCE_ARCHIVE_PATH}")

if(EXISTS "${WAVELINK_SOURCE_ARCHIVE_PATH}")
    # QRC is XML; escape paths so relocating to a folder containing '&' works.
    file(TO_CMAKE_PATH "${WAVELINK_SOURCE_ARCHIVE_PATH}" WAVELINK_SOURCE_ARCHIVE_QRC_PATH)
    string(REPLACE "&" "&amp;" WAVELINK_SOURCE_ARCHIVE_QRC_PATH "${WAVELINK_SOURCE_ARCHIVE_QRC_PATH}")
    string(REPLACE "<" "&lt;" WAVELINK_SOURCE_ARCHIVE_QRC_PATH "${WAVELINK_SOURCE_ARCHIVE_QRC_PATH}")
    string(REPLACE ">" "&gt;" WAVELINK_SOURCE_ARCHIVE_QRC_PATH "${WAVELINK_SOURCE_ARCHIVE_QRC_PATH}")
    set(WAVELINK_OPTIONAL_SOURCE_QRC
        "${CMAKE_CURRENT_BINARY_DIR}/optional_source.qrc")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/source_optional.qrc.in"
        "${WAVELINK_OPTIONAL_SOURCE_QRC}"
        @ONLY
    )
    list(APPEND WAVELINK_RESOURCE_FILES "${WAVELINK_OPTIONAL_SOURCE_QRC}")
endif()

include_directories(
    src/update
    src/core
    src/protocol
    src/hardware
    src/dsp
    src/ui
    src/utils
    src/3rdparty
)

set(WAVELINK_UPDATE_FEED_URL "" CACHE STRING "HTTPS signed update manifest URL")
set(WAVELINK_UPDATE_PUBLIC_KEY "" CACHE STRING "Base64 Windows BCRYPT_RSAPUBLIC_BLOB")
# Public channel settings only. Never put the signing PRIVATE key in this file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/update/channel.cmake")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../deploy/update/channel.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/../deploy/update/channel.cmake")
endif()
if(WAVELINK_UPDATE_FEED_URL AND NOT WAVELINK_UPDATE_FEED_URL MATCHES "^https://[A-Za-z0-9._~:/?&=%+-]+$")
    message(FATAL_ERROR "Update feed must be an ASCII HTTPS URL without quotes")
endif()
if(WAVELINK_UPDATE_PUBLIC_KEY AND NOT WAVELINK_UPDATE_PUBLIC_KEY MATCHES "^[A-Za-z0-9+/=]+$")
    message(FATAL_ERROR "Update public key must be base64")
endif()
configure_file(src/update/updateconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/updateconfig.h" @ONLY)
add_library(WaveLinkUpdates STATIC src/update/updatemanager.h src/update/updatemanager.cpp
    src/ui/versiondialog.h src/ui/versiondialog.cpp)
target_link_libraries(WaveLinkUpdates PUBLIC Qt5::Widgets Qt5::Concurrent)
target_include_directories(WaveLinkUpdates PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
    target_link_libraries(WaveLinkUpdates PRIVATE bcrypt winhttp)
    add_executable(WaveLinkUpdateLauncher WIN32 src/update/updatelauncher.cpp)
    target_compile_definitions(WaveLinkUpdateLauncher PRIVATE _WIN32_WINNT=0x0601)
    set_target_properties(WaveLinkUpdateLauncher PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_link_libraries(WaveLinkUpdateLauncher PRIVATE bcrypt shell32)
    if(MINGW)
        target_link_options(WaveLinkUpdateLauncher PRIVATE -static -static-libgcc -static-libstdc++)
    elseif(MSVC)
        set_property(TARGET WaveLinkUpdateLauncher PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

add_library(WaveLinkProtocols STATIC
    src/ui/statuspanelmodel.h src/ui/statuspanelmodel.cpp
    src/protocol/StatusMonitor.h
    src/protocol/StatusMonitor.cpp
    src/protocol/StatusTextMonitor.h
    src/protocol/StatusTextMonitor.cpp
    src/protocol/ProtocolChecksum.h
    src/protocol/ProtocolChecksum.cpp
    src/protocol/ProtocolSampleGenerator.h
    src/protocol/ProtocolSampleGenerator.cpp
    src/protocol/ProtocolConfigTransfer.h
    src/protocol/ProtocolConfigTransfer.cpp
    src/protocol/ProtocolParser.cpp
    src/protocol/ProtocolParser.h
    src/protocol/ProtocolProfile.cpp
    src/protocol/ProtocolProfile.h
    src/protocol/ProtocolProfileStore.cpp
    src/protocol/ProtocolProfileStore.h
    src/protocol/ProtocolParserFactory.cpp
    src/protocol/ProtocolParserFactory.h
    src/protocol/CustomProtocolParser.cpp
    src/protocol/CustomProtocolParser.h
)
target_link_libraries(WaveLinkProtocols PUBLIC Qt5::Core)
target_include_directories(WaveLinkProtocols PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol")
add_library(WaveLinkStatusUi STATIC
    src/ui/statusitemdialog.h src/ui/statusitemdialog.cpp
    src/ui/statusmonitorwindow.h src/ui/statusmonitorwindow.cpp)
target_link_libraries(WaveLinkStatusUi PUBLIC WaveLinkProtocols Qt5::Widgets)

add_library(WaveLinkScopeMeasurements STATIC src/dsp/scopemeasurements.h src/dsp/scopemeasurements.cpp)
target_link_libraries(WaveLinkScopeMeasurements PUBLIC Qt5::Core)
target_include_directories(WaveLinkScopeMeasurements PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp")

add_executable(WaveLink WIN32
    src/main.cpp
    src/core/mainwindow.cpp
    src/core/mainwindow.h
    src/core/mainwindow_connection.cpp
    src/core/mainwindow_plot.cpp
    src/core/mainwindow_storage.cpp
    src/core/mainwindow_offline.cpp
    src/core/mainwindow_analysis.cpp
    src/core/mainwindow_layout.cpp
    src/core/mainwindow_settings.cpp
    src/core/mainwindow_help.cpp
    src/core/mainwindow_update.cpp
    src/core/mainwindow_demo.cpp
    src/core/mainwindow_ble.cpp
    src/core/mainwindow_protocol.cpp
    src/core/mainwindow_status.cpp
    src/hardware/ble/bleipc.h
    src/hardware/ble/blesendpolicy.h
    src/hardware/ble/blehostclient.h
    src/hardware/ble/blehostclient.cpp
    src/hardware/ble/bleprofilestore.h
    src/hardware/ble/bleprofilestore.cpp
    src/hardware/ble/blefilesource.h
    src/hardware/ble/blefilesource.cpp
    src/hardware/ble/blestreamworker.h
    src/hardware/ble/blestreamworker.cpp
    src/ui/bledialogs.h
    src/ui/bledialogs.cpp
    src/ui/systemsettingsdialog.h
    src/ui/systemsettingsdialog.cpp
    src/core/systempreferences.h
    src/ui/protocoleditorwidget.cpp
    src/ui/protocoleditorwidget.h
    src/ui/protocolchecksumhelp.cpp
    src/ui/protocolchecksumhelp.h
    src/ui/protocolmanagerdialog.cpp
    src/ui/protocolmanagerdialog.h
    src/ui/protocolsampleedit.h
    src/ui/protocolsampleedit.cpp
    src/core/commandpanelwindow.cpp
    src/core/commandpanelwindow.h
    src/core/commandswitchconfig.h
    src/core/demodatagenerator.cpp
    src/core/demodatagenerator.h
    src/hardware/serialhandler.cpp
    src/hardware/serialhandler.h
    src/hardware/networkhandler.cpp
    src/hardware/networkhandler.h
    src/hardware/csvrecordingwriter.cpp
    src/hardware/csvrecordingwriter.h
    src/hardware/wlrrecordingwriter.cpp
    src/hardware/wlrrecordingwriter.h
    src/hardware/offlinewavestore.cpp
    src/hardware/offlinewavestore.h
    src/hardware/plotframebuffer.h
    src/3rdparty/qcustomplot.cpp
    src/3rdparty/qcustomplot.h
    src/dsp/filter.cpp
    src/dsp/filter.h
    src/dsp/dataprocessor.cpp
    src/dsp/dataprocessor.h
    src/ui/rangeslider.cpp
    src/ui/waveformgraph.cpp
    src/ui/waveformgraph.h
    src/ui/plotmeasurementcontroller.cpp
    src/ui/plotmeasurementcontroller.h
    src/ui/rangeslider.h
    src/ui/plotscrollarea.h
    src/dsp/ffthelper.h
    src/ui/thememanager.cpp
    src/ui/thememanager.h
    src/utils/debuglogger.cpp
    src/utils/debuglogger.h
    src/ui/helpdialog.cpp
    src/ui/helpdialog.h
    "${CMAKE_CURRENT_BINARY_DIR}/app_version.rc"
    ${WAVELINK_RESOURCE_FILES}
)

target_link_libraries(WaveLink PRIVATE
    WaveLinkScopeMeasurements
    WaveLinkUpdates
    WaveLinkStatusUi
    WaveLinkProtocols
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::SerialPort
    Qt5::Network
    Qt5::PrintSupport
    Qt5::OpenGL
    Qt5::Concurrent
    opengl32
    psapi
    dbghelp
)
if(TARGET WaveLinkUpdateLauncher)
    add_dependencies(WaveLink WaveLinkUpdateLauncher)
endif()

# Enable QCustomPlot OpenGL support
target_compile_definitions(WaveLink PRIVATE
    QCUSTOMPLOT_USE_OPENGL
    WAVELINK_VERSION="${PROJECT_VERSION}"
    WAVELINK_ENABLE_BLE=$<BOOL:${WAVELINK_ENABLE_BLE}>
)

# The helper has its own MSVC build cache. Never link it into the MinGW app.
# Build deploy/Build_BLE_Host.bat once before configuring the main project.
string(TOLOWER "${CMAKE_BUILD_TYPE}" WAVELINK_BLE_CONFIGURATION)
set(WAVELINK_BLE_HOST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../build/ble-helper-msvc-${WAVELINK_BLE_CONFIGURATION}/${CMAKE_BUILD_TYPE}/WaveLinkBleHost.exe"
    CACHE FILEPATH "Prebuilt Windows BLE host executable")
if(WAVELINK_ENABLE_BLE AND EXISTS "${WAVELINK_BLE_HOST_PATH}")
    add_custom_target(WaveLinkBleHostDeploy ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WAVELINK_BLE_HOST_PATH}" "$<TARGET_FILE_DIR:WaveLink>/WaveLinkBleHost.exe"
        DEPENDS WaveLink "${WAVELINK_BLE_HOST_PATH}"
        COMMENT "Deploying the Windows BLE host next to WaveLink")
elseif(WAVELINK_ENABLE_BLE)
    message(STATUS "BLE host has not been built yet. Run deploy/Build_BLE_Host.bat and reconfigure to deploy it.")
endif()

option(WAVELINK_BUILD_PROTOCOL_TESTS "Build protocol model, persistence and parser regression tests" OFF)
if(WAVELINK_BUILD_PROTOCOL_TESTS)
    add_executable(WaveLinkOfflineStoreTest tests/offlinewavestoretest.cpp src/hardware/offlinewavestore.cpp src/dsp/filter.cpp)
    target_include_directories(WaveLinkOfflineStoreTest PRIVATE src/hardware src/dsp)
    target_link_libraries(WaveLinkOfflineStoreTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkOfflineStore COMMAND WaveLinkOfflineStoreTest)
    set_tests_properties(WaveLinkOfflineStore PROPERTIES TIMEOUT 45)
    enable_testing()
    add_executable(WaveLinkScopeMeasurementTest tests/scopemeasurementtest.cpp)
    target_link_libraries(WaveLinkScopeMeasurementTest PRIVATE WaveLinkScopeMeasurements)
    add_test(NAME WaveLinkScopeMeasurement COMMAND WaveLinkScopeMeasurementTest)
    add_executable(WaveLinkUpdateTest tests/updatetest.cpp tests/updatetest.qrc)
    target_link_libraries(WaveLinkUpdateTest PRIVATE WaveLinkUpdates WaveLinkProtocols advapi32)
    target_compile_definitions(WaveLinkUpdateTest PRIVATE WAVELINK_UPDATE_TOOLS="${CMAKE_CURRENT_SOURCE_DIR}/../deploy/update")
    add_executable(WaveLinkUpdateLauncherTest WIN32 src/update/updatelauncher.cpp)
    set_target_properties(WaveLinkUpdateLauncherTest PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
    target_compile_definitions(WaveLinkUpdateLauncherTest PRIVATE _WIN32_WINNT=0x0601 WAVELINK_UPDATE_LAUNCHER_TEST=1)
    target_link_libraries(WaveLinkUpdateLauncherTest PRIVATE bcrypt shell32)
    add_test(NAME WaveLinkUpdate COMMAND WaveLinkUpdateTest "$<TARGET_FILE:WaveLinkUpdateLauncherTest>" "${CMAKE_CURRENT_BINARY_DIR}/artifacts/update")
    add_test(NAME WaveLinkUpdateHiDpi COMMAND WaveLinkUpdateTest "$<TARGET_FILE:WaveLinkUpdateLauncherTest>" "${CMAKE_CURRENT_BINARY_DIR}/artifacts/update-hidpi")
    set_tests_properties(WaveLinkUpdate PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)
    set_tests_properties(WaveLinkUpdateHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 90)
    add_dependencies(WaveLinkUpdateTest WaveLinkUpdateLauncher WaveLinkUpdateLauncherTest)
    add_executable(WaveLinkProtocolStoreTest tests/protocolstoretest.cpp)
    target_link_libraries(WaveLinkProtocolStoreTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolStore COMMAND WaveLinkProtocolStoreTest)
    set_tests_properties(WaveLinkProtocolStore PROPERTIES TIMEOUT 30)
    add_executable(WaveLinkCustomProtocolTest tests/customprotocoltest.cpp)
    add_executable(WaveLinkProtocolDiagnosticsTest tests/protocoldiagnosticstest.cpp)
    target_link_libraries(WaveLinkProtocolDiagnosticsTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolDiagnostics COMMAND WaveLinkProtocolDiagnosticsTest)
    set_tests_properties(WaveLinkProtocolDiagnostics PROPERTIES TIMEOUT 30)
    add_executable(WaveLinkProtocolBinarySyncTest tests/protocolbinarysynctest.cpp)
    target_link_libraries(WaveLinkProtocolBinarySyncTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolBinarySync COMMAND WaveLinkProtocolBinarySyncTest)
    set_tests_properties(WaveLinkProtocolBinarySync PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkProtocolDynamicTest tests/protocoldynamicarraytest.cpp src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkProtocolDynamicTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolDynamic COMMAND WaveLinkProtocolDynamicTest)
    set_tests_properties(WaveLinkProtocolDynamic PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkProtocolDelimitedTest tests/protocoldelimitedtest.cpp)
    target_link_libraries(WaveLinkProtocolDelimitedTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolDelimited COMMAND WaveLinkProtocolDelimitedTest)
    set_tests_properties(WaveLinkProtocolDelimited PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkProtocolKeyValueTest tests/protocolkeyvaluetest.cpp)
    target_link_libraries(WaveLinkProtocolKeyValueTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkProtocolKeyValue COMMAND WaveLinkProtocolKeyValueTest)
    set_tests_properties(WaveLinkProtocolKeyValue PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkProtocolAdvancedTest tests/protocoladvancedtest.cpp)
    target_link_libraries(WaveLinkProtocolAdvancedTest PRIVATE WaveLinkProtocols)
    # Compile the exact C99 examples shown in the checksum help with this kit.
    target_compile_definitions(WaveLinkProtocolAdvancedTest PRIVATE WAVELINK_C_TEST_COMPILER="${CMAKE_CXX_COMPILER}")
    add_test(NAME WaveLinkProtocolAdvanced COMMAND WaveLinkProtocolAdvancedTest)
    set_tests_properties(WaveLinkProtocolAdvanced PROPERTIES TIMEOUT 45)
    target_link_libraries(WaveLinkCustomProtocolTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkCustomProtocol COMMAND WaveLinkCustomProtocolTest)
    add_executable(WaveLinkProtocolUiTest tests/protocoluitest.cpp
        src/ui/protocoleditorwidget.cpp src/ui/protocoleditorwidget.h
        src/ui/protocolchecksumhelp.cpp src/ui/protocolchecksumhelp.h
        src/ui/protocolmanagerdialog.cpp src/ui/protocolmanagerdialog.h
        src/ui/protocolsampleedit.cpp src/ui/protocolsampleedit.h)
    target_link_libraries(WaveLinkProtocolUiTest PRIVATE WaveLinkProtocols Qt5::Widgets advapi32)
    add_test(NAME WaveLinkProtocolUi COMMAND WaveLinkProtocolUiTest)
    set_tests_properties(WaveLinkProtocolUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    add_test(NAME WaveLinkProtocolUiHiDpi COMMAND WaveLinkProtocolUiTest)
    set_tests_properties(WaveLinkProtocolUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    # These input tests temporarily use (and restore) the system clipboard.
    # Serialize their clipboard access even when ctest runs other tests in parallel.
    set_tests_properties(WaveLinkProtocolUi WaveLinkProtocolUiHiDpi PROPERTIES RESOURCE_LOCK WaveLinkClipboard)
    add_executable(WaveLinkCommandPanelListTest tests/commandpanellisttest.cpp
        src/core/commandpanelwindow.cpp src/core/commandpanelwindow.h)
    target_link_libraries(WaveLinkCommandPanelListTest PRIVATE Qt5::Widgets advapi32)
    add_executable(WaveLinkReceiveConsoleBufferTest tests/receiveconsolebuffertest.cpp)
    target_include_directories(WaveLinkReceiveConsoleBufferTest PRIVATE src/core)
    target_link_libraries(WaveLinkReceiveConsoleBufferTest PRIVATE Qt5::Widgets)
    add_test(NAME WaveLinkReceiveConsoleBuffer COMMAND WaveLinkReceiveConsoleBufferTest)
    set_tests_properties(WaveLinkReceiveConsoleBuffer PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    add_executable(WaveLinkCommandInteractionTest tests/commandinteractiontest.cpp src/core/commandpanelwindow.cpp src/core/commandpanelwindow.h)
    target_link_libraries(WaveLinkCommandInteractionTest PRIVATE Qt5::Widgets)
    add_test(NAME WaveLinkCommandInteraction COMMAND WaveLinkCommandInteractionTest)
    add_test(NAME WaveLinkCommandInteractionHiDpi COMMAND WaveLinkCommandInteractionTest)
    set_tests_properties(WaveLinkCommandInteraction PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandInteractionHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandInteraction WaveLinkCommandInteractionHiDpi PROPERTIES RESOURCE_LOCK WaveLinkCommandMenuArtifacts)
    add_executable(WaveLinkCommandSliderTest tests/commandslidertest.cpp src/core/commandpanelwindow.cpp src/core/commandpanelwindow.h)
    target_link_libraries(WaveLinkCommandSliderTest PRIVATE WaveLinkProtocols Qt5::Widgets advapi32)
    add_test(NAME WaveLinkCommandSlider COMMAND WaveLinkCommandSliderTest)
    add_executable(WaveLinkCommandSwitchTest tests/commandswitchtest.cpp src/core/commandpanelwindow.cpp src/core/commandpanelwindow.h)
    target_link_libraries(WaveLinkCommandSwitchTest PRIVATE WaveLinkProtocols Qt5::Widgets advapi32)
    add_executable(WaveLinkCommandConfigRecoveryTest tests/commandconfigrecoverytest.cpp src/core/commandpanelwindow.cpp src/core/commandpanelwindow.h)
    target_link_libraries(WaveLinkCommandConfigRecoveryTest PRIVATE WaveLinkProtocols Qt5::Widgets advapi32)
    add_test(NAME WaveLinkCommandConfigRecovery COMMAND WaveLinkCommandConfigRecoveryTest)
    add_test(NAME WaveLinkCommandConfigRecoveryHiDpi COMMAND WaveLinkCommandConfigRecoveryTest)
    set_tests_properties(WaveLinkCommandConfigRecovery PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandConfigRecoveryHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    add_test(NAME WaveLinkCommandSwitch COMMAND WaveLinkCommandSwitchTest)
    add_test(NAME WaveLinkCommandSwitchHiDpi COMMAND WaveLinkCommandSwitchTest)
    set_tests_properties(WaveLinkCommandSwitch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandSwitchHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandSwitch WaveLinkCommandSwitchHiDpi PROPERTIES RESOURCE_LOCK WaveLinkClipboard)
    add_test(NAME WaveLinkCommandSliderHiDpi COMMAND WaveLinkCommandSliderTest)
    set_tests_properties(WaveLinkCommandSlider PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandSliderHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandSlider WaveLinkCommandSliderHiDpi PROPERTIES RESOURCE_LOCK WaveLinkClipboard)
    add_test(NAME WaveLinkCommandPanelList COMMAND WaveLinkCommandPanelListTest)
    add_test(NAME WaveLinkCommandPanelListHiDpi COMMAND WaveLinkCommandPanelListTest)
    set_tests_properties(WaveLinkCommandPanelList PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
    set_tests_properties(WaveLinkCommandPanelListHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 30)
    add_executable(WaveLinkProtocolLiveTest tests/protocollivetest.cpp
        src/hardware/serialhandler.cpp src/hardware/serialhandler.h
        src/hardware/networkhandler.cpp src/hardware/networkhandler.h
        src/hardware/ble/blestreamworker.cpp src/hardware/ble/blestreamworker.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkProtocolLiveTest PRIVATE WaveLinkProtocols Qt5::SerialPort Qt5::Network advapi32)
    target_compile_definitions(WaveLinkProtocolLiveTest PRIVATE WAVELINK_PROTOCOL_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/protocolfixtures")
    add_test(NAME WaveLinkProtocolLive COMMAND WaveLinkProtocolLiveTest)
    add_test(NAME WaveLinkAuditReliabilityUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkAuditReliabilityUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkAuditReliabilityUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_AUDIT_RELIABILITY_UI_ONLY=1" TIMEOUT 30)
    set_tests_properties(WaveLinkAuditReliabilityUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_AUDIT_RELIABILITY_UI_ONLY=1" TIMEOUT 30)
    set_tests_properties(WaveLinkProtocolLive PROPERTIES TIMEOUT 45)
    get_target_property(WAVELINK_PROTOCOL_UI_SOURCES WaveLink SOURCES)
    list(REMOVE_ITEM WAVELINK_PROTOCOL_UI_SOURCES src/main.cpp)
    add_executable(WaveLinkProtocolLiveUiTest tests/protocolliveuitest.cpp ${WAVELINK_PROTOCOL_UI_SOURCES})
    get_target_property(WAVELINK_PROTOCOL_UI_LIBRARIES WaveLink LINK_LIBRARIES)
    target_link_libraries(WaveLinkProtocolLiveUiTest PRIVATE ${WAVELINK_PROTOCOL_UI_LIBRARIES} advapi32)
    target_compile_definitions(WaveLinkProtocolLiveUiTest PRIVATE QCUSTOMPLOT_USE_OPENGL WAVELINK_ENABLE_BLE=1 WAVELINK_VERSION="${PROJECT_VERSION}")
    set_target_properties(WaveLinkProtocolLiveUiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/protocol-ui-test")
    add_test(NAME WaveLinkProtocolLiveUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkScopeMeasurementUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/scope")
    add_test(NAME WaveLinkScopeMeasurementUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/scope-hidpi")
    set_tests_properties(WaveLinkScopeMeasurementUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_SCOPE_MEASUREMENT_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkScopeMeasurementUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_SCOPE_MEASUREMENT_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkOfflineHistoryUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/offline-history")
    add_test(NAME WaveLinkOfflineHistoryUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/offline-history-hidpi")
    set_tests_properties(WaveLinkOfflineHistoryUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_OFFLINE_HISTORY_UI_ONLY=1" TIMEOUT 90)
    set_tests_properties(WaveLinkOfflineHistoryUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_OFFLINE_HISTORY_UI_ONLY=1" TIMEOUT 90)
    add_test(NAME WaveLinkPagedOfflineUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/paged-offline")
    add_test(NAME WaveLinkPagedOfflineUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/paged-offline-hidpi")
    set_tests_properties(WaveLinkPagedOfflineUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_PAGED_OFFLINE_UI_ONLY=1" TIMEOUT 120)
    set_tests_properties(WaveLinkPagedOfflineUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_PAGED_OFFLINE_UI_ONLY=1" TIMEOUT 120)
    add_test(NAME WaveLinkPlotReadoutUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/plot-readouts")
    add_test(NAME WaveLinkPlotBackgroundUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/plot-backgrounds")
    add_test(NAME WaveLinkPlotBackgroundUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/plot-backgrounds-hidpi")
    set_tests_properties(WaveLinkPlotBackgroundUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_PLOT_BACKGROUND_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkPlotBackgroundUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_PLOT_BACKGROUND_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkPlotReadoutUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/plot-readouts-hidpi")
    set_tests_properties(WaveLinkPlotReadoutUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_PLOT_READOUT_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkPlotReadoutUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_PLOT_READOUT_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkDetachedRemovalUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkDetachedRemovalUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkDetachedRemovalUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_DETACHED_REMOVAL_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkDetachedRemovalUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_DETACHED_REMOVAL_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkHighFrequencyUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/high-frequency")
    add_test(NAME WaveLinkHighFrequencyUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/high-frequency-hidpi")
    set_tests_properties(WaveLinkHighFrequencyUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_HIGH_FREQUENCY_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkHighFrequencyUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_HIGH_FREQUENCY_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkStatusLiveUi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkStatusLiveUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_STATUS_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkStatusLiveUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkStatusLiveUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_STATUS_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkStatusLiveUi WaveLinkStatusLiveUiHiDpi PROPERTIES RESOURCE_LOCK WaveLinkStatusArtifacts)
    add_test(NAME WaveLinkProtocolLiveUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkProtocolLiveUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    # The complete software-rendered HiDPI workflow takes ~76 seconds on the
    # validation machine; short focused UI tests retain their own limits.
    set_tests_properties(WaveLinkProtocolLiveUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 120)
    set_tests_properties(WaveLinkProtocolLiveUi WaveLinkProtocolLiveUiHiDpi PROPERTIES RESOURCE_LOCK WaveLinkClipboard)
    add_test(NAME WaveLinkRangeSliderTheme COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkRangeSliderThemeHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkRangeSliderTheme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_RANGE_SLIDER_UI_ONLY=1" TIMEOUT 30)
    set_tests_properties(WaveLinkRangeSliderThemeHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_RANGE_SLIDER_UI_ONLY=1" TIMEOUT 30)
    add_test(NAME WaveLinkTimeRangeSpan COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkTimeRangeSpanHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkTimeRangeSpan PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_TIME_RANGE_UI_ONLY=1" TIMEOUT 30)
    set_tests_properties(WaveLinkTimeRangeSpanHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_TIME_RANGE_UI_ONLY=1" TIMEOUT 30)
    add_test(NAME WaveLinkRecordingShapeUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkReceiveCacheUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkReceiveHighRateUi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkReceiveHighRateUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_RECEIVE_HIGH_RATE_UI_ONLY=1" TIMEOUT 90)
    add_test(NAME WaveLinkConsoleInteractionUi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkConsoleInteractionUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_CONSOLE_UI_ONLY=1" TIMEOUT 45 RESOURCE_LOCK WaveLinkClipboard)
    add_test(NAME WaveLinkSystemSettingsUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/system-settings")
    add_test(NAME WaveLinkSystemSettingsUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/system-settings")
    set_tests_properties(WaveLinkSystemSettingsUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_SYSTEM_SETTINGS_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkSystemSettingsUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_SYSTEM_SETTINGS_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkSystemSettingsUi WaveLinkSystemSettingsUiHiDpi PROPERTIES RESOURCE_LOCK WaveLinkClipboard)
    add_test(NAME WaveLinkReceiveCacheUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkInterfaceHintsUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkInterfaceHintsUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkInterfaceHintsUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_INTERFACE_HINTS_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkInterfaceHintsUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_INTERFACE_HINTS_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkReceiveCacheUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_RECEIVE_CACHE_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkReceiveCacheUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_RECEIVE_CACHE_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkCommandSwitchLiveUi COMMAND WaveLinkProtocolLiveUiTest)
    add_test(NAME WaveLinkCommandSwitchLiveUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkCommandSwitchLiveUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_COMMAND_SWITCH_UI_ONLY=1" TIMEOUT 45)
    set_tests_properties(WaveLinkCommandSwitchLiveUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_COMMAND_SWITCH_UI_ONLY=1" TIMEOUT 45)
    add_test(NAME WaveLinkRecordingShapeUiHiDpi COMMAND WaveLinkProtocolLiveUiTest)
    set_tests_properties(WaveLinkRecordingShapeUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_RECORDING_SHAPE_UI_ONLY=1" TIMEOUT 30)
    set_tests_properties(WaveLinkRecordingShapeUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_RECORDING_SHAPE_UI_ONLY=1" TIMEOUT 30)
endif()

option(WAVELINK_BUILD_INTERNAL_TESTS "Build WaveLink internal safety tests" OFF)
option(WAVELINK_BUILD_BLE_TESTS "Build BLE process transport tests (no radio access)" OFF)
if(WAVELINK_BUILD_BLE_TESTS)
    enable_testing()
    add_executable(WaveLinkBleProfileStoreTest tests/bleprofilestoretest.cpp src/hardware/ble/bleprofilestore.cpp)
    target_link_libraries(WaveLinkBleProfileStoreTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkBleProfileStore COMMAND WaveLinkBleProfileStoreTest)
    add_executable(WaveLinkBleFakeHost tests/blefakehost.cpp)
    target_link_libraries(WaveLinkBleFakeHost PRIVATE Qt5::Core)
    if(TARGET WaveLinkProtocolLiveUiTest)
        add_dependencies(WaveLinkProtocolLiveUiTest WaveLinkBleFakeHost)
        add_test(NAME WaveLinkBleQuickUi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/ble-quick" "$<TARGET_FILE:WaveLinkBleFakeHost>")
        add_test(NAME WaveLinkBleQuickUiHiDpi COMMAND WaveLinkProtocolLiveUiTest "${CMAKE_CURRENT_BINARY_DIR}/artifacts/ble-quick" "$<TARGET_FILE:WaveLinkBleFakeHost>")
        set_tests_properties(WaveLinkBleQuickUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;WAVELINK_BLE_QUICK_UI_ONLY=1" TIMEOUT 30)
        set_tests_properties(WaveLinkBleQuickUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5;WAVELINK_BLE_QUICK_UI_ONLY=1" TIMEOUT 30)
    endif()
    add_executable(WaveLinkBleTransportTest tests/bletransporttest.cpp src/hardware/ble/blehostclient.cpp src/hardware/ble/blehostclient.h
        src/hardware/ble/blefilesource.cpp src/hardware/ble/blefilesource.h)
    target_link_libraries(WaveLinkBleTransportTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkBleTransport COMMAND WaveLinkBleTransportTest "$<TARGET_FILE:WaveLinkBleFakeHost>")
    add_executable(WaveLinkBleFileSourceTest tests/blefilesourcetest.cpp
        src/hardware/ble/blefilesource.cpp src/hardware/ble/blefilesource.h)
    target_link_libraries(WaveLinkBleFileSourceTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkBleFileSource COMMAND WaveLinkBleFileSourceTest "$<TARGET_FILE:WaveLinkBleFakeHost>")
    if(EXISTS "${WAVELINK_BLE_HOST_PATH}")
        add_test(NAME WaveLinkBleNativeFileSource COMMAND WaveLinkBleFileSourceTest "${WAVELINK_BLE_HOST_PATH}" --native)
    endif()
    add_executable(WaveLinkBleStreamWorkerTest tests/blestreamworkertest.cpp
        src/hardware/ble/blestreamworker.cpp src/hardware/ble/blestreamworker.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkBleStreamWorkerTest PRIVATE WaveLinkProtocols)
    add_test(NAME WaveLinkBleStreamWorker COMMAND WaveLinkBleStreamWorkerTest)
    add_executable(WaveLinkBleHostProbeTest tests/blehostprobetest.cpp)
    target_link_libraries(WaveLinkBleHostProbeTest PRIVATE Qt5::Core)
    if(EXISTS "${WAVELINK_BLE_HOST_PATH}")
        add_test(NAME WaveLinkBleHostProbe COMMAND WaveLinkBleHostProbeTest "${WAVELINK_BLE_HOST_PATH}")
    endif()
endif()
option(WAVELINK_BUILD_BLE_UI_TESTS "Build BLE UI smoke tests with a fake helper" OFF)
if(WAVELINK_BUILD_BLE_UI_TESTS AND WAVELINK_BUILD_BLE_TESTS)
    get_target_property(WAVELINK_UI_TEST_SOURCES WaveLink SOURCES)
    list(REMOVE_ITEM WAVELINK_UI_TEST_SOURCES src/main.cpp)
    add_executable(WaveLinkBleUiTest tests/bleuismoketest.cpp ${WAVELINK_UI_TEST_SOURCES})
    set_target_properties(WaveLinkBleUiTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ble-ui-test")
    get_target_property(WAVELINK_UI_TEST_LIBRARIES WaveLink LINK_LIBRARIES)
    target_link_libraries(WaveLinkBleUiTest PRIVATE ${WAVELINK_UI_TEST_LIBRARIES} advapi32)
    target_compile_definitions(WaveLinkBleUiTest PRIVATE QCUSTOMPLOT_USE_OPENGL WAVELINK_ENABLE_BLE=1 WAVELINK_VERSION="${PROJECT_VERSION}")
    add_dependencies(WaveLinkBleUiTest WaveLinkBleFakeHost)
    add_custom_command(TARGET WaveLinkBleUiTest POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:WaveLinkBleFakeHost>" "$<TARGET_FILE_DIR:WaveLinkBleUiTest>/WaveLinkBleHost.exe")
    add_test(NAME WaveLinkBleUi COMMAND WaveLinkBleUiTest)
    add_test(NAME WaveLinkBleUiHiDpi COMMAND WaveLinkBleUiTest)
    set_tests_properties(WaveLinkBleUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(WaveLinkBleUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5")
    set_tests_properties(WaveLinkBleUi WaveLinkBleUiHiDpi PROPERTIES TIMEOUT 45 RESOURCE_LOCK "ble_ui_snapshots;WaveLinkClipboard")
endif()
option(WAVELINK_BUILD_PLOTBUFFER_TESTS "Build plotting mailbox and scroll-layout tests" OFF)
if(WAVELINK_BUILD_PLOTBUFFER_TESTS)
    enable_testing()
    add_executable(WaveLinkWaveformPerformanceTest tests/waveformperformancetest.cpp
        src/ui/waveformgraph.cpp src/3rdparty/qcustomplot.cpp src/3rdparty/qcustomplot.h)
    target_link_libraries(WaveLinkWaveformPerformanceTest PRIVATE Qt5::Widgets Qt5::PrintSupport)
    add_test(NAME WaveLinkWaveformPerformance COMMAND WaveLinkWaveformPerformanceTest)
    set_tests_properties(WaveLinkWaveformPerformance PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_executable(WaveLinkPlotBufferTest tests/plotframebuffertest.cpp)
    target_link_libraries(WaveLinkPlotBufferTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkPlotBuffer COMMAND WaveLinkPlotBufferTest)
    add_executable(WaveLinkPlotScrollTest tests/plotscrollareatest.cpp)
    target_link_libraries(WaveLinkPlotScrollTest PRIVATE Qt5::Widgets)
    add_test(NAME WaveLinkPlotScroll COMMAND WaveLinkPlotScrollTest)
    set_tests_properties(WaveLinkPlotScroll PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
if(WAVELINK_BUILD_INTERNAL_TESTS)
    enable_testing()
    add_executable(WaveLinkQtFontWarningFilterTest tests/qtfontwarningfiltertest.cpp)
    target_link_libraries(WaveLinkQtFontWarningFilterTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkQtFontWarningFilter COMMAND WaveLinkQtFontWarningFilterTest)
    set_tests_properties(WaveLinkQtFontWarningFilter PROPERTIES TIMEOUT 15)
    add_executable(WaveLinkTextMenuTranslationTest tests/textmenutranslationtest.cpp)
    target_link_libraries(WaveLinkTextMenuTranslationTest PRIVATE Qt5::Widgets)
    add_test(NAME WaveLinkTextMenuTranslation COMMAND WaveLinkTextMenuTranslationTest)
    set_tests_properties(WaveLinkTextMenuTranslation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(WaveLinkStatusMonitorTest tests/statusmonitortest.cpp)
    add_executable(WaveLinkStatusConfigTest tests/statusconfigtest.cpp)
    target_link_libraries(WaveLinkStatusConfigTest PRIVATE WaveLinkProtocols Qt5::Core advapi32)
    target_compile_definitions(WaveLinkStatusConfigTest PRIVATE WAVELINK_PROTOCOL_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/protocolfixtures")
    add_test(NAME WaveLinkStatusConfig COMMAND WaveLinkStatusConfigTest)
    target_link_libraries(WaveLinkStatusMonitorTest PRIVATE WaveLinkProtocols Qt5::Core)
    add_test(NAME WaveLinkStatusMonitor COMMAND WaveLinkStatusMonitorTest)
    add_executable(WaveLinkStatusEventTest tests/statuseventtest.cpp)
    target_link_libraries(WaveLinkStatusEventTest PRIVATE WaveLinkProtocols Qt5::Core)
    add_test(NAME WaveLinkStatusEvent COMMAND WaveLinkStatusEventTest)
    add_executable(WaveLinkStatusUiTest tests/statusuitest.cpp)
    target_link_libraries(WaveLinkStatusUiTest PRIVATE WaveLinkStatusUi Qt5::Widgets)
    add_test(NAME WaveLinkStatusUi COMMAND WaveLinkStatusUiTest)
    add_test(NAME WaveLinkStatusUiHiDpi COMMAND WaveLinkStatusUiTest)
    set_tests_properties(WaveLinkStatusUi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 45)
    set_tests_properties(WaveLinkStatusUiHiDpi PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=1.5" TIMEOUT 45)
    set_tests_properties(WaveLinkStatusUi WaveLinkStatusUiHiDpi PROPERTIES RESOURCE_LOCK WaveLinkStatusArtifacts)
    add_executable(WaveLinkStatusTextTest tests/statustexttest.cpp
        src/hardware/serialhandler.cpp src/hardware/networkhandler.cpp src/hardware/ble/blestreamworker.cpp
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkStatusTextTest PRIVATE WaveLinkProtocols Qt5::SerialPort Qt5::Network)
    add_test(NAME WaveLinkStatusText COMMAND WaveLinkStatusTextTest)
    set_tests_properties(WaveLinkStatusText PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkFileTransferEtaTest tests/filetransferetatest.cpp)
    target_link_libraries(WaveLinkFileTransferEtaTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkFileTransferEta COMMAND WaveLinkFileTransferEtaTest)
    add_executable(WaveLinkDataSafetyAuditTest tests/datasafetyaudittest.cpp
        src/hardware/serialhandler.cpp src/hardware/serialhandler.h
        src/hardware/networkhandler.cpp src/hardware/networkhandler.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkDataSafetyAuditTest PRIVATE WaveLinkProtocols Qt5::Core Qt5::SerialPort Qt5::Network)
    add_test(NAME WaveLinkDataSafetyAudit COMMAND WaveLinkDataSafetyAuditTest)
    set_tests_properties(WaveLinkDataSafetyAudit PROPERTIES TIMEOUT 45)
    add_executable(WaveLinkTcpBindingTest tests/tcpbindingtest.cpp
        src/hardware/networkhandler.cpp src/hardware/networkhandler.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkTcpBindingTest PRIVATE WaveLinkProtocols Qt5::Network)
    add_test(NAME WaveLinkTcpBinding COMMAND WaveLinkTcpBindingTest)
    set_tests_properties(WaveLinkTcpBinding PROPERTIES TIMEOUT 30)
    add_executable(WaveLinkUdpTest tests/udptest.cpp
        src/hardware/networkhandler.cpp src/hardware/networkhandler.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkUdpTest PRIVATE WaveLinkProtocols Qt5::Network)
    add_test(NAME WaveLinkUdp COMMAND WaveLinkUdpTest)
    set_tests_properties(WaveLinkUdp PROPERTIES TIMEOUT 30)
    add_executable(WaveLinkRawRecordingTest tests/rawrecordingtest.cpp
        src/hardware/serialhandler.cpp src/hardware/serialhandler.h
        src/hardware/networkhandler.cpp src/hardware/networkhandler.h
        src/hardware/ble/blestreamworker.cpp src/hardware/ble/blestreamworker.h
        src/hardware/csvrecordingwriter.cpp src/hardware/wlrrecordingwriter.cpp)
    target_link_libraries(WaveLinkRawRecordingTest PRIVATE WaveLinkProtocols Qt5::SerialPort Qt5::Network)
    target_compile_definitions(WaveLinkRawRecordingTest PRIVATE WAVELINK_PROTOCOL_EXAMPLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/protocolfixtures")
    add_test(NAME WaveLinkRawRecording COMMAND WaveLinkRawRecordingTest)
    set_tests_properties(WaveLinkRawRecording PROPERTIES TIMEOUT 45)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/serialsafetytest.cpp")
        add_executable(WaveLinkSerialSafetyTest
            tests/serialsafetytest.cpp
            src/hardware/serialhandler.cpp
            src/hardware/serialhandler.h
            src/hardware/csvrecordingwriter.cpp
            src/hardware/csvrecordingwriter.h
            src/hardware/wlrrecordingwriter.cpp
            src/hardware/wlrrecordingwriter.h
        )
        target_link_libraries(WaveLinkSerialSafetyTest PRIVATE
            WaveLinkProtocols
            Qt5::Core
            Qt5::SerialPort
        )
        add_test(NAME WaveLinkSerialSafety COMMAND WaveLinkSerialSafetyTest)
    endif()

    add_executable(WaveLinkRecordingSafetyTest
        tests/recordingsafetytest.cpp
        src/hardware/csvrecordingwriter.cpp
        src/hardware/csvrecordingwriter.h
        src/hardware/wlrrecordingwriter.cpp
        src/hardware/wlrrecordingwriter.h
    )
    target_link_libraries(WaveLinkRecordingSafetyTest PRIVATE Qt5::Core)
    add_test(NAME WaveLinkRecordingSafety COMMAND WaveLinkRecordingSafetyTest)

    add_executable(WaveLinkHelpDialogThemeTest
        tests/helpdialogthemetest.cpp
        src/ui/helpdialog.cpp
        src/ui/helpdialog.h
        src/utils/debuglogger.cpp
        src/utils/debuglogger.h
    )
    target_link_libraries(WaveLinkHelpDialogThemeTest PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
    target_compile_definitions(WaveLinkHelpDialogThemeTest PRIVATE WAVELINK_VERSION="${PROJECT_VERSION}")
    add_test(NAME WaveLinkHelpDialogTheme COMMAND WaveLinkHelpDialogThemeTest)
    set_tests_properties(WaveLinkHelpDialogTheme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# Optional: Set executable icon for Windows
if(WIN32)
    # add_executable(WaveLink WIN32 ...) if you don't want the console window to appear
    # We keep console for now to see qDebug output
endif()
