# Shared transport headers; source URL and identity come from the versioned lock.
include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../third_party/core-dependencies.lock.json" cpn_dependencies)
string(JSON cpn_dependency_schema GET "${cpn_dependencies}" schema_version)
if(NOT cpn_dependency_schema EQUAL 1)
    message(FATAL_ERROR "Unsupported Core dependency lock schema")
endif()
foreach(cpn_key IN ITEMS version numeric_version url archive_sha256 license_file license_sha256)
    string(JSON cpn_boost_${cpn_key} GET "${cpn_dependencies}" boost ${cpn_key})
endforeach()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../${cpn_boost_license_file}" cpn_boost_license_actual)
if(NOT cpn_boost_license_actual STREQUAL cpn_boost_license_sha256)
    message(FATAL_ERROR "Bundled Boost license differs from the dependency lock")
endif()
# SOURCE_SUBDIR intentionally excludes Boost's library build: Asio/Beast are header-only.
FetchContent_Declare(cpn_boost
    URL "${cpn_boost_url}"
    URL_HASH "SHA256=${cpn_boost_archive_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR cpn-header-only)
FetchContent_MakeAvailable(cpn_boost)
file(STRINGS "${cpn_boost_SOURCE_DIR}/boost/version.hpp" cpn_boost_version_line
    REGEX "^#define BOOST_VERSION [0-9]+$")
if(NOT cpn_boost_version_line STREQUAL "#define BOOST_VERSION ${cpn_boost_numeric_version}")
    message(FATAL_ERROR "Boost source version differs from the dependency lock")
endif()
file(SHA256 "${cpn_boost_SOURCE_DIR}/LICENSE_1_0.txt" cpn_boost_source_license)
if(NOT cpn_boost_source_license STREQUAL cpn_boost_license_sha256)
    message(FATAL_ERROR "Boost source license differs from the dependency lock")
endif()
add_library(cpn_transport_headers INTERFACE)
target_include_directories(cpn_transport_headers SYSTEM INTERFACE "${cpn_boost_SOURCE_DIR}")
target_compile_definitions(cpn_transport_headers INTERFACE BOOST_ALL_NO_LIB)
if(WIN32)
    target_link_libraries(cpn_transport_headers INTERFACE ws2_32 mswsock)
endif()
