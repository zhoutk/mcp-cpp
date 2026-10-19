# Conan 2.x recipe for mcp-cpp. Build with:
#   conan build packaging/conan --version=0.3.1
# or consume it after exporting:
#   conan export packaging/conan --version=0.3.1
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy


class McpCppConan(ConanFile):
    name = "mcp-cpp"
    # The actual source of truth is version.txt; the export command pins it.
    settings = "os", "compiler", "build_type", "arch"
    options = {"with_tests": [True, False], "shared": [True, False]}
    default_options = {"with_tests": False, "shared": False}
    exports = "version.txt"

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.variables["MCP_BUILD_TESTS"] = self.options.with_tests
        tc.variables["MCP_BUILD_EXAMPLES"] = False
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", src=self.recipe_folder, dst=os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # httplib and zjson are vendored; nothing beyond the system platform
        # libraries (ws2_32 on Windows / Threads on POSIX) is needed, and
        # mcpConfig.cmake resolves those for find_package() consumers.
        self.cpp_info.set_property("cmake_file_name", "mcp")
        self.cpp_info.set_property("cmake_target_name", "mcp::mcp")
