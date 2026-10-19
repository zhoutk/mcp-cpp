# vcpkg port for mcp-cpp. Install with:
#   vcpkg install --overlay-ports=packaging/vcpkg mcp-cpp
# httplib and zjson are vendored in the repo's thirds/ directory, so the only
# build inputs are the sources themselves.
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/zhoutk/mcp-cpp
    REF v0.3.1
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMCP_INSTALL=ON
        -DMCP_BUILD_TESTS=OFF
        -DMCP_BUILD_EXAMPLES=OFF
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/mcp PACKAGE_NAME mcp)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
