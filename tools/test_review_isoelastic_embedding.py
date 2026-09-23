"""Compile bounded resource byte arrays and prove byte-exact reconstruction (MSVC or C++17)."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ReviewIsoelasticEmbeddingTest(unittest.TestCase):
    def roundtrip(self, payload):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            source = work / 'input.txt'
            header = work / 'ReviewIsoelasticData.h'
            source.write_bytes(payload)
            subprocess.run(['cmake', '-DINPUT=' + str(source), '-DOUTPUT=' + str(header), '-P', str(ROOT / 'cmake/GenerateReviewIsoelastic.cmake')], check=True, capture_output=True)
            generated = header.read_text()
            literals = re.findall(r'inline constexpr unsigned char kReviewIsoelasticChunk\d+\[\] = \{(.*)\};', generated)
            self.assertGreater(len(literals), 1)
            self.assertTrue(all(literal.count('0x') <= 4096 for literal in literals))
            self.assertEqual(generated.count('data.append('), len(literals))
            cpp = work / 'roundtrip.cpp'
            cpp.write_text('#include "ReviewIsoelasticData.h"\n#include <fstream>\nint main(int argc,char** argv){auto data=reviewIsoelasticData();std::ofstream out(argv[1],std::ios::binary);out.write(data.data(),data.size());return out ? 0 : 1;}\n')
            executable = work / ('roundtrip.exe' if os.name == 'nt' else 'roundtrip')
            if os.name == 'nt':
                command = ['cl', '/nologo', '/std:c++17', '/EHsc', str(cpp), '/Fe:' + str(executable)]
            else:
                command = [os.environ.get('CXX', 'c++'), '-std=c++17', str(cpp), '-o', str(executable)]
            subprocess.run(command, cwd=work, check=True, capture_output=True)
            output = work / 'roundtrip.bin'
            subprocess.run([str(executable), str(output)], check=True)
            self.assertEqual(output.read_bytes(), payload)

    def test_entire_scientific_reference_is_byte_exact(self):
        self.roundtrip((ROOT / 'resources/isoelastic_curve/scaled_isoelastic_data_6.16-4.24.txt').read_bytes())

    def test_chunk_boundaries_escaping_and_line_endings(self):
        self.roundtrip((b'123.456\t0.00012 4.24; "quoted" \\path\r\n' * 2500) + bytes(range(256)) + b'last line without newline')


if __name__ == '__main__':
    unittest.main()
