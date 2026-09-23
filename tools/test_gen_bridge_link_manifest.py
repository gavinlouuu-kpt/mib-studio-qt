"""Portable regression for the Windows VS-generated link manifest reader."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class LinkManifestXmlTest(unittest.TestCase):
    def test_escaped_defines_and_paths_roundtrip_without_entity_fragments(self):
        with tempfile.TemporaryDirectory() as root:
            build = Path(root)
            (build / 'tests').mkdir()
            (build / 'src/backend').mkdir(parents=True)
            (build / 'vendor & deps/lib').mkdir(parents=True)
            (build / 'vendor & deps/bin').mkdir()
            library = build / 'vendor & deps/lib/native&codec.lib'
            library.touch()
            reference = '''<Project>
<ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
<Link><AdditionalDependencies>../vendor &amp; deps/lib/native&amp;codec.lib;kernel32.lib;%(AdditionalDependencies)</AdditionalDependencies>
<AdditionalLibraryDirectories>../vendor &amp; deps/lib;%(AdditionalLibraryDirectories)</AdditionalLibraryDirectories></Link>
<ClCompile><PreprocessorDefinitions>MIB_VERSION=&quot;1.2.3&quot;;EMPTY=&quot;&quot;;CMAKE_INTDIR=&quot;Release&quot;;%(PreprocessorDefinitions)</PreprocessorDefinitions>
<AdditionalIncludeDirectories>../headers &amp; includes;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories></ClCompile>
</ItemDefinitionGroup></Project>'''
            (build / 'tests/mib_backend_smoke_test.vcxproj').write_text(reference)
            backend = '''<Project>
<ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release|x64'">
<ClCompile><PreprocessorDefinitions>LABEL=&quot;A&amp;B&quot;;MIB_HAS_COREMOR=0;%(PreprocessorDefinitions)</PreprocessorDefinitions>
<ExternalIncludeDirectories>../../external &amp; headers;%(ExternalIncludeDirectories)</ExternalIncludeDirectories></ClCompile>
</ItemDefinitionGroup>
<ItemGroup><ClCompile><AdditionalIncludeDirectories Condition="'$(Configuration)'=='Release'">../../per-file &amp; headers;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
<AdditionalIncludeDirectories Condition="'$(Configuration)'=='Debug'">../../debug-only</AdditionalIncludeDirectories></ClCompile></ItemGroup>
</Project>'''
            (build / 'src/backend/mib_backend.vcxproj').write_text(backend)
            subprocess.run([sys.executable, str(Path(__file__).with_name('gen_bridge_link_manifest.py')), '--build-dir', str(build)], check=True, capture_output=True, text=True)
            manifest = json.loads((build / 'mib-bridge-link-manifest.json').read_text())
            self.assertEqual(manifest['libs'], ['native&codec', 'kernel32'])
            self.assertEqual(manifest['defines'], ['MIB_VERSION="1.2.3"', 'EMPTY=""', 'LABEL="A&B"', 'MIB_HAS_COREMOR=0'])
            for name in ['headers & includes', 'external & headers', 'per-file & headers']:
                self.assertIn(str(build / name), manifest['include_dirs'])
            self.assertNotIn(str(build / 'debug-only'), manifest['include_dirs'])
            self.assertIn(str(library.parent), manifest['lib_dirs'])
            self.assertIn(str(build / 'vendor & deps/bin'), manifest['runtime_dirs'])
            self.assertFalse(any('&quot' in value or '&amp' in value for value in manifest['defines']))


if __name__ == '__main__':
    unittest.main()
