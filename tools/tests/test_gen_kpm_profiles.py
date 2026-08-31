import copy
import json
import pathlib
import re
import tempfile
import unittest

from tools import gen_kpm_profiles as generator


ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILES = ROOT / "kpm" / "profiles" / "profiles.json"


class ProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = generator.load_profiles(PROFILES)[0]

    def test_plk110_offsets_are_exact(self):
        profile = self.profile
        self.assertEqual(profile["id"], "oneplus-plk110-a16-b4999618-d05")
        self.assertEqual(profile["task"]["pid"], 1800)
        self.assertEqual(profile["task"]["tgid"], 1804)
        self.assertEqual(profile["task"]["comm"], 2320)
        self.assertEqual(profile["task"]["start_boottime"], 2112)
        self.assertEqual(profile["cred"]["uid"], 8)
        self.assertEqual(profile["debug"]["brps"], 6)
        self.assertEqual(profile["debug"]["wrps"], 4)
        self.assertEqual(profile["layout"]["work_struct_size"], 32)
        self.assertEqual(profile["layout"]["work_data"], 0)
        self.assertEqual(profile["layout"]["work_entry"], 8)
        self.assertEqual(profile["layout"]["work_func"], 24)
        self.assertEqual(profile["layout"]["pt_regs_size"], 336)
        self.assertEqual(profile["layout"]["pt_regs_pstate"], 264)
        self.assertEqual(profile["symbols"]["flush_work"], "flush_work")
        self.assertEqual(
            profile["symbols"].get("synchronize_rcu_tasks"),
            "synchronize_rcu_tasks",
        )
        self.assertEqual(
            profile["kernel"]["ack_commit"],
            "b2a876903b495c444a94b16f50d1463ffe953957",
        )
        self.assertEqual(profile["kernel"]["task_comm_len"], 16)
        self.assertEqual(profile["kernel"]["linux_banner_capacity"], 256)
        self.assertEqual(
            profile["kpatch"]["control_path"],
            "/data/adb/modules/KPatch-Next/bin/kpatch",
        )
        self.assertEqual(profile["kpatch"]["module_name"], "stackplz-kpm")
        self.assertEqual(profile["kpatch"]["safe_unload_symbol"], "kpm_safe_unload_v1")
        self.assertEqual(profile["kpatch"]["memcpy_kfunc"], "kf_memcpy")
        self.assertTrue(profile["kpatch"]["kfunc_exports_are_pointers"])
        self.assertEqual(profile["hooks"]["finish_task_switch_args"], 1)
        self.assertEqual(profile["hooks"]["breakpoint_handler_args"], 3)
        self.assertFalse(profile["quirks"]["safe_unload_required"])
        self.assertTrue(profile["quirks"]["linux_banner_prefix_ok"])

    def test_plk110_maps_abi_is_exact(self):
        profile = self.profile
        self.assertIn("maps", profile)
        self.assertEqual(profile["maps"]["max_snapshot_bytes"], 2 * 1024 * 1024)
        self.assertEqual(profile["maps"]["max_chunk_bytes"], 1536)
        self.assertEqual(profile["maps"]["seq_file_size"], 136)
        self.assertEqual(profile["maps"]["seq_private"], 128)
        self.assertEqual(profile["maps"]["proc_maps_private_size"], 120)
        self.assertEqual(profile["maps"]["proc_iter"], 24)
        self.assertEqual(profile["maps"]["vma_iterator_size"], 72)
        self.assertEqual(profile["maps"]["mas_status"], 56)
        self.assertEqual(profile["maps"]["mm_mt"], 64)
        self.assertEqual(profile["maps"]["vma_end"], 8)
        self.assertEqual(profile["maps"]["show_map_vma_args"], 2)
        self.assertEqual(profile["symbols"]["show_map_vma"], "show_map_vma")
        self.assertEqual(profile["symbols"]["vmalloc_noprof"], "vmalloc_noprof")

    def test_maps_symbols_are_required_by_the_profile_table(self):
        profile = self._profile_with_maps_abi()
        del profile["symbols"]["show_map_vma"]
        with self.assertRaisesRegex(ValueError, "missing key.*show_map_vma"):
            generator.validate_profile(profile)

    def test_maps_layout_offsets_are_width_checked(self):
        profile = self._profile_with_maps_abi()
        profile["maps"]["seq_private"] = profile["maps"]["seq_file_size"] - 4
        with self.assertRaisesRegex(ValueError, "maps.seq_private"):
            generator.validate_profile(profile)

    def test_maps_formatter_abi_and_chunk_limit_are_closed(self):
        bad_abi = self._profile_with_maps_abi()
        bad_abi["maps"]["show_map_vma_args"] = 3
        with self.assertRaisesRegex(ValueError, "maps.show_map_vma_args"):
            generator.validate_profile(bad_abi)

        bad_chunk = self._profile_with_maps_abi()
        bad_chunk["maps"]["max_chunk_bytes"] = 2048
        with self.assertRaisesRegex(ValueError, "maps.max_chunk_bytes"):
            generator.validate_profile(bad_chunk)

    def _profile_with_maps_abi(self):
        profile = copy.deepcopy(self.profile)
        profile["maps"] = {
            "max_snapshot_bytes": 2 * 1024 * 1024,
            "max_chunk_bytes": 1536,
            "seq_file_size": 136,
            "seq_buf": 0,
            "seq_size": 8,
            "seq_from": 16,
            "seq_count": 24,
            "seq_pad_until": 32,
            "seq_private": 128,
            "proc_maps_private_size": 120,
            "proc_task": 8,
            "proc_mm": 16,
            "proc_iter": 24,
            "vma_iterator_size": 72,
            "mas_tree": 0,
            "mas_index": 8,
            "mas_last": 16,
            "mas_node": 24,
            "mas_status": 56,
            "mm_struct_size": 1216,
            "mm_mt": 64,
            "vma_struct_size": 256,
            "vma_start": 0,
            "vma_end": 8,
            "mas_start_node": 1,
            "ma_start_status": 1,
            "show_map_vma_args": 2,
        }
        profile["symbols"].update({
            "show_map_vma": "show_map_vma",
            "find_vma": "find_vma",
            "mas_walk": "mas_walk",
            "get_task_mm": "get_task_mm",
            "mmput": "mmput",
            "mmap_read_lock_killable": "mmap_read_lock_killable",
            "mmap_read_unlock": "mmap_read_unlock",
            "rust_helper_get_task_struct": "rust_helper_get_task_struct",
            "rust_helper_put_task_struct": "rust_helper_put_task_struct",
            "vmalloc_noprof": "vmalloc_noprof",
            "vfree": "vfree",
        })
        return profile

    def test_unknown_nested_key_is_rejected(self):
        profile = copy.deepcopy(self.profile)
        profile["task"]["guessed_offset"] = 4
        with self.assertRaisesRegex(ValueError, "unknown key.*guessed_offset"):
            generator.validate_profile(profile)

    def test_negative_or_out_of_bounds_offsets_are_rejected(self):
        negative = copy.deepcopy(self.profile)
        negative["task"]["pid"] = -1
        with self.assertRaisesRegex(ValueError, "task.pid"):
            generator.validate_profile(negative)

        outside = copy.deepcopy(self.profile)
        outside["task"]["comm"] = outside["kernel"]["task_struct_size"]
        with self.assertRaisesRegex(ValueError, "task.comm"):
            generator.validate_profile(outside)

        bad_work = copy.deepcopy(self.profile)
        bad_work["layout"]["work_func"] = bad_work["layout"]["work_struct_size"]
        with self.assertRaisesRegex(ValueError, "layout.work_func"):
            generator.validate_profile(bad_work)

        bad_regs = copy.deepcopy(self.profile)
        bad_regs["layout"]["pt_regs_pstate"] = bad_regs["layout"]["pt_regs_size"]
        with self.assertRaisesRegex(ValueError, "layout.pt_regs_pstate"):
            generator.validate_profile(bad_regs)

    def test_duplicate_profile_ids_are_rejected(self):
        document = {
            "schema_version": 1,
            "default_profile": self.profile["id"],
            "profiles": [self.profile, self.profile],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "profiles.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate profile id"):
                generator.load_profiles(path)

    def test_rendering_is_sorted_and_escapes_strings(self):
        second = copy.deepcopy(self.profile)
        second["id"] = "aaa-profile"
        second["product"]["model"] = 'quoted"model'
        c_output = generator.render_c([self.profile, second], self.profile["id"])
        go_output = generator.render_go([self.profile, second], self.profile["id"])
        self.assertLess(c_output.index("aaa-profile"), c_output.index(self.profile["id"]))
        self.assertIn('quoted\\"model', c_output)
        self.assertIn('#define SPZ_DEFAULT_DEVICE_PROFILE_ID', c_output)
        self.assertLess(go_output.index("aaa-profile"), go_output.index(self.profile["id"]))
        self.assertIn('quoted\\"model', go_output)
        self.assertIn(f'const DefaultProfileID = "{self.profile["id"]}"', go_output)

    def test_go_output_uses_stable_acronyms_and_gofmt_alignment(self):
        output = generator.render_go([self.profile], self.profile["id"])
        self.assertEqual(generator.go_name("max_cpus"), "MaxCPUs")
        self.assertEqual(generator.go_name("attr_bp_type"), "AttrBPType")
        self.assertIn("\tManufacturer   string\n", output)
        self.assertIn('\t\t\tManufacturer:   "OnePlus",\n', output)

    def test_check_mode_never_rewrites_stale_output(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "generated.go"
            path.write_text("stale\n", encoding="utf-8")
            self.assertFalse(generator.write_or_check(path, "fresh\n", check=True))
            self.assertEqual(path.read_text(encoding="utf-8"), "stale\n")
            self.assertTrue(generator.write_or_check(path, "fresh\n", check=False))
            self.assertEqual(path.read_bytes(), b"fresh\n")

    def test_unknown_default_profile_is_rejected(self):
        document = {
            "schema_version": 1,
            "default_profile": "missing-profile",
            "profiles": [self.profile],
        }
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "profiles.json"
            path.write_text(json.dumps(document), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "default_profile"):
                generator.load_profiles(path)

    def test_makefile_sdk_commit_matches_current_profile(self):
        makefile = (ROOT / "kpm" / "Makefile").read_text(encoding="utf-8")
        match = re.search(r"EXPECTED_KP_COMMIT := ([0-9a-f]{40})", makefile)
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), self.profile["kpatch"]["sdk_commit"])


if __name__ == "__main__":
    unittest.main()
