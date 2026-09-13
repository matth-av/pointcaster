add_library(pointcaster_ui
    initialisation.cc
    layout_saver.cc
    models/workspace_model.cc
    models/enum_adapters.h
    models/settings_page_registry.h
    models/settings_page_registry.cc
    models/stream_channel_model.h
    models/stream_channel_model.cc
    models/log_model.h
    models/log_model.cc
    models/commands/device_config_commands.cc
    models/session_recorder_model.cc
    models/operator_adapter.cc
    models/session_adapter.cc
    models/stream_source.h
    models/stream_source.cc
    window/view_factory.cc
    window/tab_bar_view.cc
    window/tab_drop.cc
    window/tab_drop_indicators.cc
)
add_library(pointcaster::ui ALIAS pointcaster_ui)

target_include_directories(pointcaster_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

add_subdirectory(qml)

find_package(Corrade CONFIG REQUIRED REQUIRED Main Utility)
find_package(QtAwesome CONFIG REQUIRED)

# TODO ??
find_package(TBB REQUIRED)

target_link_libraries(pointcaster_ui PRIVATE
    pointcaster::core
    pointcaster::config
    pointcaster::pipeline
    pointcaster::app_settings
    pointcaster::plugins
    PointcasterQml
    # TODO which of these aren't already brought in by above
    Corrade::Main Corrade::Utility
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::OpenGL
    Qt6::Quick Qt6::QuickControls2 Qt6::Quick3D
    KDAB::kddockwidgets
    QtAwesome::QtAwesome
    #
    TBB::tbb
)

#TODO why are these two here that target the parent project??
target_link_libraries(pointcaster PRIVATE Qt6::Gui)

qt_import_qml_plugins(pointcaster)

# for our generated headers that connect configuration structs <-> qml
set(CONFIG_ADAPTER_INPUTS
    config/file_config.h
    config/network_config.h
    config/transform_config.h
    config/color_transform_config.h
    config/sequence_config.h
    config/group_sequence_config.h
    session/session_config.h
    camera/camera_config.h
    camera/look_at_camera_config.h
    core/codec/codec_config.h
    point_streamer/point_streamer_config.h
    pipeline/concurrent_operator_pipeline_config.h
    plugins/devices/device_group_config.h
    publishers/publishers_config.h
    publishers/message_streamer/message_streamer_config.h
    publishers/mqtt/mqtt_client_config.h
    publishers/osc/osc_sender_config.h
    # TODO we need some faculty to create these adapters
    # without them being expressed in the main module here...
    # i guess the UI adapters need to be generated inside the 
    # device CMakeLists.txt not here
    plugins/devices/orbbec/orbbec_device_config.h
    plugins/devices/ply/ply_device_config.h
    plugins/operators/fringe_removal/fringe_removal_config.h
    plugins/operators/cluster_extraction/cluster_extraction_config.h
    plugins/operators/range_filter/range_filter_config.h
)

add_qml_adapters(
    TARGETS pointcaster_ui
    CONFIG_INPUTS ${CONFIG_ADAPTER_INPUTS}
    TEMPLATES_DIR "${CMAKE_SOURCE_DIR}/src/templates"
    OUT_DIR "${CMAKE_BINARY_DIR}/generated/config_adapters"
)

target_precompile_headers(pointcaster_ui PRIVATE
    <string>
    <string_view>
    <vector>
    <memory>
    <mutex>
    <functional>
    <chrono>
    <algorithm>
    <optional>
    <QObject>
    <QString>
    <QVariant>
    <QUrl>
    <QCoreApplication>
    <QThread>
    <QMetaObject>
    <QUndoCommand>
    <QByteArray>
    <QQuick3DGeometry>
)
