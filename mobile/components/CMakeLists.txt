# SPDX-FileCopyrightText: 2024 Carl Schwan <carl@carlschwan.eu>
# SPDX-License-Identifier: BSD-2-Clause

ecm_add_qml_module(okularplugin 
    URI "org.kde.okular"
    VERSION 2.0
    GENERATE_PLUGIN_SOURCE
    DEPENDENCIES
        org.kde.kirigami
)

target_sources(okularplugin PRIVATE
    ${CMAKE_SOURCE_DIR}/gui/guiutils.cpp
    ${CMAKE_SOURCE_DIR}/gui/tocmodel.cpp
    ${CMAKE_SOURCE_DIR}/gui/signaturemodel.cpp
    ${CMAKE_SOURCE_DIR}/gui/signatureguiutils.cpp
    ${CMAKE_SOURCE_DIR}/gui/certificatemodel.cpp
    ${CMAKE_SOURCE_DIR}/gui/pagepainter.cpp
    ${CMAKE_SOURCE_DIR}/gui/debug_ui.cpp
    ${CMAKE_SOURCE_DIR}/gui/recentitemsmodel.cpp
    pageitem.cpp
    textselection.cpp
    dictionarylookup.cpp
    dictionarylookup.h
    ${CMAKE_SOURCE_DIR}/part/mdxdictionary.cpp
    documentitem.cpp
    thumbnailitem.cpp
    okularsingleton.cpp
    types.cpp
    welcomeitem.cpp
)

kconfig_add_kcfg_files(okularplugin GENERATE_MOC ${CMAKE_SOURCE_DIR}/conf/settings_mobile.kcfgc)

ecm_target_qml_sources(okularplugin SOURCES
    DocumentView.qml
)

ecm_target_qml_sources(okularplugin PRIVATE PATH private SOURCES
    private/PageView.qml
    private/SelectionMagnifier.qml
)

target_link_libraries(okularplugin PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Core
    Qt6::Concurrent
    Qt6::Xml
    Qt6::Svg
    KF6::Bookmarks
    KF6::I18n
    KF6::Service
    okularcore
)

okular_enable_mdict(okularplugin)
if(MDICT_FOUND)
    target_link_libraries(okularplugin PRIVATE ${MDICT_LIBRARY} ${MDICT_MINIZ_LIBRARY} ${MDICT_BASE64_LIBRARY})
endif()

ecm_finalize_qml_module(okularplugin DESTINATION ${KDE_INSTALL_QMLDIR} EXPORT OkularTargets)

#add_subdirectory(test)
