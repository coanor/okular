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
    ${CMAKE_SOURCE_DIR}/gui/textselectionutils.cpp
    dictionarylookup.cpp
    dictionarylookup.h
    readinghistorymodel.cpp
    readinghistorymodel.h
    ${CMAKE_SOURCE_DIR}/part/mdxdictionary.cpp
    aiassistant.cpp
    aiassistant.h
    chatgptconnection.cpp
    chatgptconnection.h
    chatgpttoken.cpp
    ${CMAKE_SOURCE_DIR}/part/aiprovider.cpp
    ${CMAKE_SOURCE_DIR}/part/aistore.cpp
    documentitem.cpp
    thumbnailitem.cpp
    okularsingleton.cpp
    types.cpp
    welcomeitem.cpp
)

target_compile_definitions(okularplugin PRIVATE HAVE_CHATGPT_AUTH=$<BOOL:${OpenSSL_FOUND}>)
if(OpenSSL_FOUND)
    target_link_libraries(okularplugin PRIVATE OpenSSL::Crypto)
endif()

kconfig_add_kcfg_files(okularplugin GENERATE_MOC ${CMAKE_SOURCE_DIR}/conf/settings_mobile.kcfgc)

ecm_target_qml_sources(okularplugin SOURCES
    DocumentView.qml
)

ecm_target_qml_sources(okularplugin PRIVATE PATH private SOURCES
    private/PageView.qml
    private/SelectionMagnifier.qml
    private/SelectionLookupTimer.qml
)

target_link_libraries(okularplugin PRIVATE
    Qt6::Quick
    Qt6::Qml
    Qt6::Core
    Qt6::Network
    KF6::ConfigCore
    Qt6::Concurrent
    Qt6::Xml
    Qt6::Svg
    KF6::Bookmarks
    KF6::I18n
    KF6::Service
    okularcore
    okularreadingdata
)

if(KF6Wallet_FOUND)
    target_link_libraries(okularplugin PRIVATE KF6::Wallet)
endif()

okular_enable_mdict(okularplugin)
if(MDICT_FOUND)
    target_link_libraries(okularplugin PRIVATE ${MDICT_LIBRARY} ${MDICT_MINIZ_LIBRARY} ${MDICT_BASE64_LIBRARY})
endif()

ecm_finalize_qml_module(okularplugin DESTINATION ${KDE_INSTALL_QMLDIR} EXPORT OkularTargets)

#add_subdirectory(test)
