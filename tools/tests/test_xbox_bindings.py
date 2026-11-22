"""What bindings.py writes into a .bind file for the straight-line prologues its docstring describes.

Each program is a few synthetic instructions assembled here, not shader data: the words are built with the same encodings the decoder
reads, so a decoder change that renames or resizes an instruction fails an expectation instead of silently changing what the analysis
sees. The classes, node kinds and width rules are the ones the module documents; the module's scan stops at the first block it cannot
follow, which is why every program here is straight-line.
"""

import hashlib
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS / "xbox"))
import bindings

SMEM_X2, SMEM_X4, SMEM_X8 = 1, 2, 3
SMEM_BUFFER_DWORD, SMEM_BUFFER_X4 = 8, 10
BUFFER_LOAD_DWORD = 12
TBUFFER_LOAD_FORMAT_XYZW = 3
IMAGE_LOAD, IMAGE_STORE, IMAGE_SAMPLE = 0, 8, 32
IMAGE_ATOMIC_ADD, IMAGE_ATOMIC_CMP_SWAP, IMAGE_GET_LOD, IMAGE_BVH = 17, 16, 96, 230
S_MOV_B32, S_SETPC_B64 = 3, 32
S_ADD_I32 = 2
S_BARRIER, S_END_PGM = 10, 1
INLINE_ZERO = 128


def smem(op, sdst=0, sbase=0, offset=0):
    return [0xF4000000 | (op << 18) | (sdst << 6) | (sbase >> 1), (0x7D << 25) | offset]


def mubuf(op, srsrc=0, vdata=0, vaddr=0, offset=0):
    return [0xE0000000 | (op << 18) | (1 << 12) | offset,
            (128 << 24) | ((srsrc // 4) << 16) | (vdata << 8) | vaddr]


def mtbuf(op, srsrc=0, vdata=0, vaddr=0):
    return [0xE8000000 | (1 << 19) | ((op & 7) << 16) | (1 << 13),
            (128 << 24) | (((op >> 3) & 1) << 21) | ((srsrc // 4) << 16) | (vdata << 8) | vaddr]


def mimg(op, srsrc=0, ssamp=0, vdata=0, vaddr=0, dim=1, dmask=0xF):
    return [0xF0000000 | ((op & 0x7F) << 18) | ((op >> 7) & 1) | (dim << 3) | (dmask << 8),
            ((ssamp // 4) << 21) | ((srsrc // 4) << 16) | (vdata << 8) | vaddr]


def sop1(op, sdst, src):
    return [0xBE800000 | (sdst << 16) | (op << 8) | src]


def sop2(op, sdst, src0, src1):
    return [0x80000000 | (op << 23) | (sdst << 16) | (src1 << 8) | src0]


def sopp(op):
    return [0xBF800000 | (op << 16)]


def nodes(lines):
    return [tuple(line.split()) for line in lines if line.startswith("node ")]


def descs(lines):
    return [tuple(line.split()) for line in lines if line.startswith("desc ")]


class AnalyseTest(unittest.TestCase):
    def analyse(self, words, nuser=16):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "7.bin")
            with open(path, "wb") as handle:
                handle.write(struct.pack("<%dI" % len(words), *words))
            return bindings.analyse(path, nuser)

    def test_the_file_starts_with_the_version_stamp(self):
        lines, _, _, _ = self.analyse([])
        self.assertEqual(lines, ["version " + bindings.VERSION])

    def test_the_stamp_hashes_the_generator_with_unix_line_endings(self):
        source = pathlib.Path(bindings.__file__).read_bytes().replace(b"\r\n", b"\n")
        self.assertEqual(bindings.VERSION, hashlib.sha256(source).hexdigest()[:12])

    def test_the_counters_report_loads_and_nodes(self):
        _, stats, loads, count = self.analyse(smem(SMEM_X4, 4, 30) + smem(SMEM_X2))
        self.assertEqual((dict(stats), loads, count), ({}, 1, 1))

    def test_a_user_sgpr_table_becomes_a_node(self):
        self.assertEqual(nodes(self.analyse(smem(SMEM_X4, 4))[0]),
                         [("node", "0", "user", "0")])

    def test_a_two_dword_load_is_a_pointer(self):
        lines = self.analyse(smem(SMEM_X2) + smem(SMEM_X4, 4, 0, 0x40))[0]
        self.assertEqual(nodes(lines), [("node", "0", "user", "0"), ("node", "1", "child", "0", "0")])

    def test_a_four_dword_load_is_not_a_pointer(self):
        self.assertEqual(len(nodes(self.analyse(smem(SMEM_X4) + smem(SMEM_X4, 4, 0, 0x40))[0])), 1)

    def test_a_pointer_must_use_the_first_dword_of_its_load(self):
        self.assertEqual(len(nodes(self.analyse(smem(SMEM_X2, 1) + smem(SMEM_X4, 4, 2, 0x40))[0])), 1)

    def test_a_moved_pointer_keeps_the_node(self):
        lines = self.analyse(smem(SMEM_X2) + sop1(S_MOV_B32, 8, 0) + smem(SMEM_X4, 4, 8, 0x40))[0]
        self.assertEqual(nodes(lines)[-1], ("node", "1", "child", "0", "0"))

    def test_a_zeroed_register_is_not_a_pointer(self):
        self.assertEqual(len(nodes(self.analyse(smem(SMEM_X2) + sop1(S_MOV_B32, 8, INLINE_ZERO)
                                                + smem(SMEM_X4, 4, 8, 0x40))[0])), 1)

    def test_a_scalar_op_forgets_the_pointer(self):
        self.assertEqual(len(nodes(self.analyse(smem(SMEM_X2) + sop2(S_ADD_I32, 0, 0, 0)
                                                + smem(SMEM_X4, 4, 0, 0x40))[0])), 1)

    def test_a_table_outside_the_user_range_is_ignored(self):
        self.assertEqual(nodes(self.analyse(smem(SMEM_X4, 4, 30))[0]), [])

    def test_a_mesh_stage_reaches_sgpr_39(self):
        self.assertEqual(nodes(self.analyse(smem(SMEM_X4, 4, 38), bindings.user_sgprs("?"))[0]),
                         [("node", "0", "user", "38")])

    def test_a_mesh_stage_stops_at_sgpr_39(self):
        self.assertEqual(nodes(self.analyse(smem(SMEM_X4, 4, 40), bindings.user_sgprs("?"))[0]), [])

    def test_an_export_half_gets_the_mesh_user_range(self):
        lines = self.analyse(smem(SMEM_X4, 4, 38) + sopp(S_BARRIER) + sop1(S_SETPC_B64, 0, 0),
                             bindings.user_sgprs("VS"))[0]
        self.assertEqual(nodes(lines), [("node", "0", "user", "38")])

    def test_a_jump_without_the_barrier_is_not_an_export_half(self):
        lines = self.analyse(smem(SMEM_X4, 4, 30) + sop1(S_SETPC_B64, 0, 0), bindings.user_sgprs("VS"))[0]
        self.assertEqual(nodes(lines), [])

    def test_a_buffer_load_binds_the_loaded_vsharp(self):
        lines = self.analyse(smem(SMEM_X4, 4) + smem(SMEM_BUFFER_DWORD, 8, 4, 0x10))[0]
        self.assertEqual(descs(lines), [("desc", "0", "0", "buf")])

    def test_a_typed_buffer_load_binds_a_buffer(self):
        lines = self.analyse(smem(SMEM_X4, 4) + mtbuf(TBUFFER_LOAD_FORMAT_XYZW, 4))[0]
        self.assertEqual(descs(lines), [("desc", "0", "0", "buf")])

    def test_a_buffer_vsharp_needs_four_dwords_from_one_load(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_X2) + mubuf(BUFFER_LOAD_DWORD, 0))[0]), [])

    def test_the_binding_offset_is_the_descriptor_offset(self):
        lines = self.analyse(smem(SMEM_X4, 0, 0, 0x20) + mubuf(BUFFER_LOAD_DWORD, 0))[0]
        self.assertEqual(descs(lines), [("desc", "0", "32", "buf")])

    def test_an_image_binds_with_its_dimension(self):
        for dim in range(4):
            lines = self.analyse(smem(SMEM_X8) + mimg(IMAGE_LOAD, dim=dim))[0]
            self.assertEqual(descs(lines), [("desc", "0", "0", "img%d" % dim)])

    def test_a_sampling_instruction_binds_the_sampler_at_its_offset(self):
        lines = self.analyse(smem(SMEM_X8) + mimg(IMAGE_SAMPLE, ssamp=4, dim=2))[0]
        self.assertEqual(descs(lines), [("desc", "0", "0", "img2"), ("desc", "0", "16", "samp")])

    def test_a_load_binds_no_sampler(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_X8) + mimg(IMAGE_LOAD))[0]),
                         [("desc", "0", "0", "img1")])

    def test_a_two_dword_atomic_binds_the_64bit_class(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_X8) + mimg(IMAGE_ATOMIC_ADD, dmask=0x3))[0]),
                         [("desc", "0", "0", "img1b")])

    def test_a_one_dword_atomic_binds_the_32bit_class(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_X8) + mimg(IMAGE_ATOMIC_ADD))[0]),
                         [("desc", "0", "0", "img1a")])

    def test_a_compare_swap_needs_four_dwords_for_the_64bit_class(self):
        wide = self.analyse(smem(SMEM_X8) + mimg(IMAGE_ATOMIC_CMP_SWAP, dmask=0xF))[0]
        narrow = self.analyse(smem(SMEM_X8) + mimg(IMAGE_ATOMIC_CMP_SWAP, dmask=0x3))[0]
        self.assertEqual((descs(wide), descs(narrow)),
                         ([("desc", "0", "0", "img1b")], [("desc", "0", "0", "img1a")]))

    def test_a_bvh_instruction_binds_nothing(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_X8) + mimg(IMAGE_BVH))[0]), [])

    def test_an_inline_vsharp_in_the_user_data_binds_nothing(self):
        self.assertEqual(descs(self.analyse(smem(SMEM_BUFFER_X4, 4, 0, 0))[0]), [])

    def test_identical_bindings_are_written_once(self):
        lines, stats = self.analyse(smem(SMEM_X8) + mimg(IMAGE_LOAD) + mimg(IMAGE_LOAD))[:2]
        self.assertEqual((descs(lines), dict(stats)), ([("desc", "0", "0", "img1")], {"img1": 1}))

    def test_bindings_are_sorted_by_node_then_offset(self):
        lines = self.analyse(smem(SMEM_X8) + mubuf(BUFFER_LOAD_DWORD, 4) + mubuf(BUFFER_LOAD_DWORD, 0))[0]
        self.assertEqual(descs(lines), [("desc", "0", "0", "buf"), ("desc", "0", "16", "buf")])


class WriteTest(unittest.TestCase):
    def test_the_bind_file_is_named_after_the_shader(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "bins"
            target = pathlib.Path(directory) / "binds"
            source.mkdir()
            target.mkdir()
            words = smem(SMEM_X4, 4) + mubuf(BUFFER_LOAD_DWORD, 4)
            (source / "7.bin").write_bytes(struct.pack("<%dI" % len(words), *words))
            stats, loads = bindings._write((str(source), str(target), "7.bin", 16))
            self.assertEqual((dict(stats), loads), ({"buf": 1}, 1))
            self.assertEqual((target / "7.bind").read_text().splitlines(),
                             ["version " + bindings.VERSION, "node 0 user 0", "desc 0 0 buf"])


class ScanTest(unittest.TestCase):
    def run_scan(self, stages):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = pathlib.Path(directory.name)
        bins, binds = root / "bins", root / "binds"
        bins.mkdir()
        words = smem(SMEM_X4, 4, 30)
        for index in stages:
            (bins / ("%d.bin" % index)).write_bytes(struct.pack("<%dI" % len(words), *words))
        (root / "manifest_full.csv").write_text(
            "idx,stage\n" + "".join("%d,%s\n" % (index, stage) for index, stage in stages.items()), encoding="utf-8")
        result = subprocess.run([sys.executable, "-I", str(TOOLS / "xbox" / "bindings.py"), str(bins), str(binds)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout, {path.name: path.read_text().splitlines() for path in binds.iterdir()}

    def test_the_scan_reads_the_stage_beside_the_bins(self):
        stdout, written = self.run_scan({7: "CS", 8: "?"})
        self.assertIn("2 shaders", stdout)
        self.assertEqual(written["7.bind"], ["version " + bindings.VERSION])
        self.assertEqual(written["8.bind"], ["version " + bindings.VERSION, "node 0 user 30"])


if __name__ == "__main__":
    unittest.main()
