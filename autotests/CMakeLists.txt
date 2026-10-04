set(OKULAR_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE STRING
    "Autotest source directory as seen by the test runtime")
add_definitions( -DKDESRCDIR="${OKULAR_TEST_DATA_DIR}/" )

include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

macro(add_conf_unittest _source)
  set(_test ${_source} ../part/editdrawingtooldialog.cpp)
  get_filename_component(_name ${_source} NAME_WE)
  add_executable( ${_name} ${_test} )
  add_test( NAME ${_name} COMMAND ${_name} )
  ecm_mark_as_test(${_name})
  target_link_libraries( ${_name}
    Qt6::Test
    Qt6::Widgets
    Qt6::Xml
    KF6::I18n
    KF6::Completion
    KF6::WidgetsAddons
  )
endmacro ()

add_conf_unittest(editdrawingtooldialogtest.cpp)

ecm_add_test(textselectiontest.cpp ../gui/textselectionutils.cpp ../part/mdxdictionary.cpp
    TEST_NAME "textselectiontest"
    LINK_LIBRARIES Qt6::Test okularcore
)
target_compile_definitions(textselectiontest PRIVATE HAVE_MDICT=0)

if(BUILD_MOBILE)
    find_package(Qt6 REQUIRED COMPONENTS QuickTest)
    ecm_add_test(selectionmagnifiertest.cpp
        TEST_NAME "selectionmagnifiertest"
        LINK_LIBRARIES Qt6::QuickTest
    )
    target_compile_definitions(selectionmagnifiertest PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/mobile")
endif()

ecm_add_test(mdxdictionarytest.cpp ../part/mdxdictionary.cpp
    TEST_NAME "mdxdictionarytest"
    LINK_LIBRARIES Qt6::Test Qt6::Widgets
)
okular_enable_mdict(mdxdictionarytest)
if(MDICT_FOUND)
    target_link_libraries(mdxdictionarytest ${MDICT_LIBRARY} ${MDICT_MINIZ_LIBRARY} ${MDICT_BASE64_LIBRARY})
endif()

if(OKULAR_USE_BUNDLED_MDICT)
    ecm_add_test(mdictparsertest.cpp
        TEST_NAME "mdictparsertest"
        LINK_LIBRARIES Qt6::Test okular_mdict ZLIB::ZLIB
    )
    target_include_directories(mdictparsertest PRIVATE ${MDICT_INCLUDE_DIR})
endif()

ecm_add_test(shelltest.cpp ../shell/shellutils.cpp
    TEST_NAME "shelltest"
    LINK_LIBRARIES Qt6::Test okularcore
)

if(Poppler_Qt6_FOUND)
    if(BUILD_MOBILE AND NOT ANDROID)
        find_package(Qt6 ${QT_REQUIRED_VERSION} REQUIRED COMPONENTS QuickTest)
        ecm_add_test(mobiledocumentviewtest.cpp
            TEST_NAME "mobiledocumentviewtest"
            LINK_LIBRARIES Qt6::QuickTest Qt6::Qml Qt6::Widgets Qt6::Network KF6::I18n
        )
        target_compile_definitions(mobiledocumentviewtest PRIVATE
            QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
            OKULAR_QML_IMPORT_PATH="${CMAKE_BINARY_DIR}/bin"
        )
        add_dependencies(mobiledocumentviewtest okularplugin okularGenerator_poppler)
    endif()

    if (BUILD_DESKTOP)
        ecm_add_test(parttest.cpp closedialoghelper.cpp
            TEST_NAME "parttest"
            LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore okularpart
        )
    endif()

    ecm_add_test(visibilitytest.cpp
        TEST_NAME "visibilitytest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(jsfunctionstest.cpp
        TEST_NAME "jsfunctionstest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(formattest.cpp
        TEST_NAME "formattest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(keystroketest.cpp
        TEST_NAME "keystroketest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )

    ecm_add_test(signunsignedfieldtest.cpp
        TEST_NAME "signunsignedfieldtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
    )
endif()

ecm_add_test(suggestedfilenametest.cpp
    TEST_NAME "suggestedfilenametest"
    LINK_LIBRARIES Qt6::Test okularcore KF6::I18n
)

ecm_add_test(documenttest.cpp
    TEST_NAME "documenttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore KF6::ThreadWeaver
)

ecm_add_test(searchtest.cpp
    TEST_NAME "searchtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(annotationstest.cpp
    TEST_NAME "annotationstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(annotationsidecartest.cpp
    TEST_NAME "annotationsidecartest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Sql okularcore
)

ecm_add_test(aiprovidertest.cpp ../part/aiprovider.cpp
    TEST_NAME "aiprovidertest"
    LINK_LIBRARIES Qt6::Test Qt6::Network
)
if(BUILD_MOBILE AND OpenSSL_FOUND)
    ecm_add_test(chatgptconnectiontest.cpp ../mobile/components/chatgptconnection.cpp ../mobile/components/chatgpttoken.cpp
        TEST_NAME "chatgptconnectiontest"
        LINK_LIBRARIES Qt6::Test Qt6::Network OpenSSL::Crypto
    )
    target_compile_definitions(chatgptconnectiontest PRIVATE HAVE_CHATGPT_AUTH=1)
endif()
if(BUILD_DESKTOP)
    ecm_add_test(aimarkdownviewtest.cpp ../part/aimarkdownview.cpp ../part/aiassets/aiassets.qrc
        TEST_NAME "aimarkdownviewtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::WebEngineWidgets KF6::I18n
    )
    ecm_add_test(annotationclipboardtest.cpp
        TEST_NAME "annotationclipboardtest"
        LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore okularpart
    )
endif()

ecm_add_test(urldetecttest.cpp
    TEST_NAME "urldetecttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml KF6::CoreAddons
)

ecm_add_test(editannotationcontentstest.cpp testingutils.cpp
    TEST_NAME "editannotationcontentstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(addremoveannotationtest.cpp testingutils.cpp
    TEST_NAME "addremoveannotationtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(translateannotationtest.cpp testingutils.cpp
    TEST_NAME "translateannotationtest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test Qt6::Xml okularcore
)

ecm_add_test(modifyannotationpropertiestest.cpp testingutils.cpp
    TEST_NAME "modifyannotationpropertiestest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(editformstest.cpp
    TEST_NAME "editformstest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(calculatetexttest.cpp
    TEST_NAME "calculatetexttest"
    LINK_LIBRARIES Qt6::Widgets Qt6::Test okularcore
)

ecm_add_test(check_distinguished_name_parser.cpp
    TEST_NAME "distinguishednameparser"
    LINK_LIBRARIES Qt6::Test)

if(BUILD_DESKTOP)
    ecm_add_test(mainshelltest.cpp ../shell/okular_main.cpp ../shell/shellutils.cpp ../shell/shell.cpp ../shell/welcomescreen.cpp ../gui/recentitemsmodel.cpp closedialoghelper.cpp
        TEST_NAME "mainshelltest"
        LINK_LIBRARIES Qt6::Test okularpart okularcore
    )
    target_compile_definitions(mainshelltest PRIVATE OKULAR_BINARY="$<TARGET_FILE:okular>")

    if (HAVE_X11)
        target_link_libraries(mainshelltest Qt6::GuiPrivate)
    endif()
endif()

if(BUILD_DESKTOP)
    ecm_add_test(annotationtoolbartest.cpp ../shell/okular_main.cpp ../shell/shellutils.cpp ../shell/shell.cpp ../shell/welcomescreen.cpp ../gui/recentitemsmodel.cpp closedialoghelper.cpp ../shell/welcomescreen.ui
        TEST_NAME "annotationtoolbartest"
        LINK_LIBRARIES Qt6::Test okularpart
    )

    if (HAVE_X11)
        target_link_libraries(annotationtoolbartest Qt6::GuiPrivate)
    endif()
endif()

ecm_add_test(generatorstest.cpp
    TEST_NAME "generatorstest"
    LINK_LIBRARIES Qt6::Test KF6::CoreAddons okularcore
)
target_compile_definitions(generatorstest PRIVATE GENERATORS_BUILD_DIR="${CMAKE_BINARY_DIR}/generators")

ecm_add_test(signatureformtest.cpp
    TEST_NAME "signatureformtest"
    LINK_LIBRARIES Qt6::Test okularcore
)

find_package(Discount)

if(Discount_FOUND)
    ecm_add_test(markdowntest.cpp ../generators/markdown/converter.cpp
        TEST_NAME "markdowntest"
        LINK_LIBRARIES Qt6::Test okularcore KF6::I18n PkgConfig::Discount
    )
endif()

ecm_add_test(toggleactionmenutest.cpp ../part/toggleactionmenu.cpp
    TEST_NAME "toggleactionmenutest"
    LINK_LIBRARIES Qt6::Test KF6::WidgetsAddons
)
