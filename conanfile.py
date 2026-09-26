from conan import ConanFile


class MibStudioQtDeps(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"

    default_options = {
        "qt/*:shared": True,
        "qt/*:qtcharts": True,
        "qt/*:qtserialport": True,
        "qt/*:with_imageformats": True,
        "opencv/*:shared": True,
        "opencv/*:with_openexr": False,
        "opencv/*:dnn": False,
        "hdf5/*:shared": True,
        "hdf5/*:enable_cxx": True,
    }

    def requirements(self):
        self.requires("qt/6.7.3")
        self.requires("spdlog/1.17.0")
        self.requires("sqlite3/3.51.0")
        self.requires("hdf5/1.14.6")
        # Direct zlib dependency (ADR 0006): the compression writer calls zlib
        # itself, not only through HDF5's deflate filter. Same range the hdf5
        # recipe declares, so it resolves to the package already in the graph.
        self.requires("zlib/[>=1.2.11 <2]")
        self.requires("opencv/4.12.0")
        self.requires("nlohmann_json/3.11.3")

        if self.settings.os == "Windows":
            self.requires("onnxruntime/1.18.1")

        if self.settings.os == "Linux":
            self.requires("xkbcommon/1.6.0", override=True)
            self.requires("wayland/1.24.0", override=True)
