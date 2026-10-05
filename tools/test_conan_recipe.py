"""Run with the Python environment containing Conan 2; no network/cache needed."""
import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest

spec = importlib.util.spec_from_file_location('mib_conan_recipe', Path(__file__).resolve().parents[1] / 'conanfile.py')
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)


class ConanRecipeTest(unittest.TestCase):
    def requirements(self, os_name, with_qt=None):
        # Real ConanFile options/Requirements, not a stand-in for Conan's Boolean parsing.
        value = recipe.MibStudioQtDeps()
        value.settings = SimpleNamespace(os=os_name)
        if with_qt is not None:
            value.options.with_qt = with_qt
        value.requirements()
        return {str(item.ref): item.override for item in value.requires.values()}

    def test_windows_default_preserves_qt_and_backend_dependencies(self):
        default = self.requirements('Windows')
        disabled = self.requirements('Windows', 'False')
        self.assertIn('qt/6.7.3', default)
        self.assertEqual(set(default) - set(disabled), {'qt/6.7.3'})
        self.assertIn('onnxruntime/1.18.1', disabled)
        self.assertEqual(self.requirements('Windows', 'True'), default)

    def test_linux_headless_omits_only_qt_and_qt_platform_overrides(self):
        default = self.requirements('Linux')
        disabled = self.requirements('Linux', False)
        self.assertEqual(set(default) - set(disabled), {'qt/6.7.3', 'xkbcommon/1.6.0', 'wayland/1.24.0'})
        self.assertTrue(default['xkbcommon/1.6.0'])
        self.assertTrue(default['wayland/1.24.0'])
        self.assertNotIn('onnxruntime/1.18.1', disabled)


if __name__ == '__main__':
    unittest.main()
