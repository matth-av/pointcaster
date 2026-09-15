
# singletons inside qml must be specified to cmake before
# configuring the qml module

set(qml_singletons
    Workspace.qml
    Theme/ThemeColors.qml
    Theme/FontAwesome.qml
    Theme/Scaling.qml
    Utils/StringUtils.qml
    Utils/TransformUtils.qml
)
foreach(qml_singleton_file IN LISTS qml_singletons)
    set_source_files_properties( ${qml_singleton_file} 
        PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
endforeach()

qt_add_qml_module(PointcasterQml
	URI Pointcaster VERSION 1.0
    QML_FILES
        ${qml_singletons}
        Controls/DragInput.qml
        Controls/DragInt.qml
        Controls/DragFloat.qml
        Controls/DragFloat3.qml
        Controls/PositionBoundsEditor.qml
        Controls/EnumSelector.qml
        Controls/FocusReleaser.qml
        Controls/IconButton.qml
        Controls/ListToggleButton.qml
        Controls/SelectedSessionLabel.qml
        Controls/InfoToolTip.qml
        Controls/PaddedScrollBar.qml
        Controls/PaddedScrollView.qml
        Controls/LogEntry.qml
        Controls/OriginGizmo.qml
        Controls/OrbitViewController.qml
        Controls/Gizmos/SelectionGizmo.qml
        Controls/Gizmos/BoundsGizmo.qml
        Controls/Gizmos/SelectionBoundsGizmo.qml
        Controls/Gizmos/RadiusGizmo.qml
        Controls/Gizmos/SelectionRadiusGizmo.qml
        Controls/Timeline.qml
        Editors/ConfigurationEditor.qml
        Editors/OperatorPipelineEditor.qml
        PointCloud/PointCloudMaterial.qml
        PointCloud/PointIndexMaterial.qml
        Windows/TabBar.qml
        Windows/TabBarBase.qml
        Windows/TabDropIndicator.qml
        Windows/DropIndicatorTile.qml
        Windows/Separator.qml
        Windows/FloatingWindow.qml
        Windows/Group.qml
        Windows/ResizeHandlerHelper.qml
        Windows/TopMenuBar.qml
        Windows/StatusBar.qml
        Windows/WorkspaceConsole.qml
        Windows/MainWindow.qml
        Windows/Settings/SettingsWindow.qml
        Windows/Settings/SettingsPage.qml
        Windows/Settings/SettingsSection.qml
        Windows/Settings/SettingsRow.qml
        Windows/Settings/GeneralSettingsPage.qml
        Windows/Settings/InterfaceSettingsPage.qml
        Windows/Settings/PerformanceSettingsPage.qml
        Windows/Settings/DiagnosticsSettingsPage.qml
        Windows/Devices/DeviceSelectionList.qml
        Windows/Devices/DevicesToolBar.qml
        Windows/Devices/DevicesWindow.qml
        Windows/Devices/DeviceControlRow.qml
        Windows/Devices/AddDeviceMenu.qml
        Windows/Sessions/SessionPropertiesWindow.qml
        Windows/Sessions/SessionCamera.qml
        Windows/Sessions/SessionView.qml
        Windows/Sessions/CentralSessionView.qml
        Windows/Sessions/SessionControls.qml
        Windows/Sessions/SessionControlCollapser.qml
        Windows/Sessions/SessionTimeline.qml
        Windows/Recording/RecordingWindow.qml
        Windows/Streaming/StreamingWindow.qml
        Windows/Streaming/StreamChannelList.qml
        Windows/Alignment/AlignmentWindow.qml
        Windows/Alignment/AlignmentView.qml
        Windows/Alignment/RefinementView.qml
        Windows/Alignment/SnapshotControls.qml
        Windows/Alignment/PickingControls.qml
        Windows/Alignment/RefinementControls.qml
        Windows/Publishers/PublishersWindow.qml
    RESOURCES
        Fonts/AtkinsonHyperlegibleNext-Light.otf
        Fonts/AtkinsonHyperlegibleNext-Regular.otf
        Fonts/AtkinsonHyperlegibleNext-Medium.otf
        Fonts/AtkinsonHyperlegibleNext-Bold.otf
        Fonts/AtkinsonHyperlegibleMono-Light.otf
        Fonts/AtkinsonHyperlegibleMono-Regular.otf
        Fonts/AtkinsonHyperlegibleMono-Medium.otf
        Fonts/AtkinsonHyperlegibleMono-Bold.otf
        PointCloud/Shaders/PointCloud.vert
        PointCloud/Shaders/PointCloud.frag
        PointCloud/Shaders/PointIndex.vert
        PointCloud/Shaders/PointSolid.frag
    SOURCES
        # TODO need to be able to add from a subdir but atm this doesn't work
        AlignmentController.h
        AlignmentController.cc
        PointCloudGeometry.h
        PointCloudGeometry.cc
        StreamInstancing.h
        StreamInstancing.cc
        color_picker.h
        color_picker.cc
        net_utils.h
        net_utils.cc
)

find_package(QtAwesome CONFIG REQUIRED)
find_package(gizmo3d CONFIG REQUIRED)

target_link_libraries(PointcasterQml PRIVATE
    pointcaster::core
    pointcaster::registration
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::OpenGL
    Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2
    Qt6::Quick3D
    Qt6::Concurrent
    KDAB::kddockwidgets
    QtAwesome::QtAwesome
    Gizmo3D::gizmo3d
)

target_include_directories(PointcasterQml 
    PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}/src")

install(DIRECTORY "${QT_QML_OUTPUT_DIRECTORY}/Pointcaster"
	DESTINATION "${CMAKE_INSTALL_QMLDIR}"
	# build artifacts that aren't needed at runtime
	PATTERN "*.lib" EXCLUDE
	PATTERN "*.exp" EXCLUDE
	PATTERN "*.qmltypes" EXCLUDE
	PATTERN "*_qml_module_dir_map.qrc" EXCLUDE
)

install(TARGETS PointcasterQml
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" EXCLUDE_FROM_ALL
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

target_precompile_headers(PointcasterQml PRIVATE
  <string>
  <string_view>
  <vector>
  <memory>
  <QObject>
  <QString>
  <QVariant>
  <QUrl>
  <QByteArray>
  <QVector3D>
  <QQuick3DGeometry>
  <QtQmlIntegration/qqmlintegration.h>
  <QCoreApplication>
)