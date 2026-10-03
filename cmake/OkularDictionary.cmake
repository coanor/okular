# SPDX-License-Identifier: BSD-2-Clause

option(OKULAR_BUNDLE_DICTIONARY "Include the ECDICT English-Chinese dictionary" ON)
set(OKULAR_DICTIONARY_FILE "" CACHE FILEPATH "Local ECDICT MDX file to bundle instead of downloading it")
set(okular_dictionary_license "${CMAKE_CURRENT_LIST_DIR}/../thirdparty/ecdict/LICENSE")
set(okular_dictionary_qrc_template "${CMAKE_CURRENT_LIST_DIR}/OkularDictionary.qrc.in")

if(OKULAR_BUNDLE_DICTIONARY AND MDICT_FOUND)
    if(OKULAR_DICTIONARY_FILE)
        if(NOT EXISTS "${OKULAR_DICTIONARY_FILE}" OR IS_DIRECTORY "${OKULAR_DICTIONARY_FILE}")
            message(FATAL_ERROR "OKULAR_DICTIONARY_FILE must name an existing MDX file")
        endif()
        set(okular_dictionary_source "${OKULAR_DICTIONARY_FILE}")
    else()
        include(FetchContent)
        if(POLICY CMP0135)
            cmake_policy(SET CMP0135 NEW)
        endif()
        FetchContent_Declare(okular_ecdict
            URL https://github.com/skywind3000/ECDICT/releases/download/1.0.28/ecdict-mdx-28.zip
            URL_HASH SHA256=b06a72a0cfc37485a0466ee62fb43137559ea75eea147d8b7715142faca2229f
        )
        FetchContent_MakeAvailable(okular_ecdict)
        file(GLOB okular_dictionary_files "${okular_ecdict_SOURCE_DIR}/*.mdx")
        list(LENGTH okular_dictionary_files dictionary_count)
        if(NOT dictionary_count EQUAL 1)
            message(FATAL_ERROR "The ECDICT archive must contain exactly one MDX file")
        endif()
        list(GET okular_dictionary_files 0 okular_dictionary_source)
    endif()
    # The release ZIP uses a GBK filename. Keep raw archive names out of the UTF-8 QRC.
    set(okular_dictionary_file "${CMAKE_CURRENT_BINARY_DIR}/dictionaries/default.mdx")
    configure_file("${okular_dictionary_source}" "${okular_dictionary_file}" COPYONLY)
endif()

function(okular_bundle_dictionary target)
    if(OKULAR_BUNDLE_DICTIONARY AND MDICT_FOUND)
        # Escape file paths for XML, including local overrides containing '&'.
        foreach(path_var okular_dictionary_file okular_dictionary_license)
            string(REPLACE "&" "&amp;" ${path_var} "${${path_var}}")
            string(REPLACE "<" "&lt;" ${path_var} "${${path_var}}")
            string(REPLACE ">" "&gt;" ${path_var} "${${path_var}}")
        endforeach()
        set(qrc "${CMAKE_CURRENT_BINARY_DIR}/${target}_dictionary.qrc")
        configure_file("${okular_dictionary_qrc_template}" "${qrc}" @ONLY)
        # MDX is already compressed. Big resources avoid compiling a huge C++ array.
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        qt_add_big_resources(dictionary_resources "${qrc}" OPTIONS --no-compress)
        target_sources(${target} PRIVATE ${dictionary_resources})
    endif()
endfunction()
