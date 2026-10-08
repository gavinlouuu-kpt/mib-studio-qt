from conan import ConanFile


class MibStudioQtDeps(ConanFile):
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"
    # review_core: the dependency graph of YOFO Review's review core only
    # (plan 2026-10-01-standalone-review-app, ADR 0014) — mib_processing +
    # mib_review_core need spdlog, HDF5, OpenCV (core / imgproc / imgcodecs /
    # videoio) and nlohmann_json; no Qt or SQLite. Libraries
    # are static so the Tauri binary carries them and the DMG / installer
    # bundle no third-party dylibs or DLLs. Used by the macos-review-core and
    # windows-review-core presets:
    #   conan install . -of build/review-core -o "&:review_core=True" ...
    # with_qt=False drops Qt from the full graph (desktop shell builds).
    options = {"with_qt": [True, False], "review_core": [True, False]}
    default_options = {
        "with_qt": True,
        "review_core": False,
        "qt/*:shared": True,
        "qt/*:qtcharts": True,
        "qt/*:qtserialport": True,
        "qt/*:with_imageformats": True,
        "opencv/*:with_openexr": False,
        "opencv/*:dnn": False,
        "hdf5/*:enable_cxx": True,
    }

    def configure(self):
        review_core = bool(self.options.review_core)
        self.options["opencv/*"].shared = not review_core
        self.options["hdf5/*"].shared = not review_core
        if review_core:
            # Only the modules the review core includes; the rest (and their
            # codec / protobuf / FFmpeg dependencies) would only cost CI time.
            # Without FFmpeg, AVI sources for mask regeneration use OpenCV's
            # built-in MJPEG reader (plus AVFoundation / Media Foundation).
            for module in ("calib3d", "features2d", "flann", "gapi", "highgui", "ml",
                           "objdetect", "photo", "stitching", "video"):
                setattr(self.options["opencv/*"], module, False)
            for feature in ("with_ffmpeg", "with_protobuf", "with_eigen", "with_quirc",
                            "with_webp", "with_jpeg2000", "with_tesseract"):
                setattr(self.options["opencv/*"], feature, False)

    def requirements(self):
        self.requires("spdlog/1.17.0")
        self.requires("hdf5/1.14.6")
        self.requires("opencv/4.12.0")
        self.requires("nlohmann_json/3.11.3")
        if self.options.review_core:
            return

        if self.options.with_qt:
            self.requires("qt/6.7.3")
        self.requires("sqlite3/3.51.0")

        if self.settings.os == "Linux" and self.options.with_qt:
            self.requires("xkbcommon/1.6.0", override=True)
            self.requires("wayland/1.24.0", override=True)
