import pathlib
import tempfile
import unittest

from tools import check_kpm_elf as checker


NM_OUTPUT = """\
                 U compat_copy_to_user
                 U hook_unwrap_remove
                 U hook_wrap
                 U kallsyms_lookup_name
                 U kf_memcpy
                 U kf_memset
                 U kpm_safe_unload_v1
                 U kpver
                 U kver
"""


class KPMELFTests(unittest.TestCase):
    def test_only_declared_kpatch_imports_are_accepted(self):
        imports = checker.parse_undefined_symbols(NM_OUTPUT)
        checker.validate_imports(imports)
        self.assertEqual(
            imports,
            {
                "compat_copy_to_user",
                "hook_unwrap_remove",
                "hook_wrap",
                "kallsyms_lookup_name",
                "kf_memcpy",
                "kf_memset",
                "kpm_safe_unload_v1",
                "kpver",
                "kver",
            },
        )

    def test_missing_safe_unload_import_is_accepted(self):
        imports = checker.parse_undefined_symbols(
            "\n".join(
                line
                for line in NM_OUTPUT.splitlines()
                if "kpm_safe_unload_v1" not in line
            )
            + "\n"
        )
        checker.validate_imports(imports)
        self.assertNotIn("kpm_safe_unload_v1", imports)

    def test_unexpected_runtime_import_is_rejected(self):
        imports = checker.parse_undefined_symbols(NM_OUTPUT + " U perf_event_create_kernel_counter\n")
        with self.assertRaisesRegex(ValueError, "unexpected import.*perf_event"):
            checker.validate_imports(imports)

    def test_required_kpm_sections_and_architecture_are_checked(self):
        header = (
            "Class:                             ELF64\n"
            "Data:                              2's complement, little endian\n"
            "Type:                              REL (Relocatable file)\n"
            "Machine:                           AArch64\n"
        )
        sections = " .text .kpm.info .kpm.init .kpm.ctl0 .kpm.exit .bss "
        checker.validate_elf_metadata(header, sections)
        with self.assertRaisesRegex(ValueError, "missing KPM section.*ctl0"):
            checker.validate_elf_metadata(header, sections.replace(".kpm.ctl0", ""))

    def test_every_import_must_have_an_sdk_export_declaration(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            files = {
                "kernel/patch/common/utils.c": "KP_EXPORT_SYMBOL(compat_copy_to_user);\n",
                "kernel/base/hook.c": (
                    "KP_EXPORT_SYMBOL(hook_wrap);\n"
                    "KP_EXPORT_SYMBOL(hook_unwrap_remove);\n"
                ),
                "kernel/base/start.c": (
                    "KP_EXPORT_SYMBOL(kallsyms_lookup_name);\n"
                    "KP_EXPORT_SYMBOL(kpver);\n"
                    "KP_EXPORT_SYMBOL(kver);\n"
                ),
                "kernel/patch/ksyms/libs.c": (
                    "KP_EXPORT_SYMBOL(kfunc(memcpy));\n"
                    "KP_EXPORT_SYMBOL(kfunc(memset));\n"
                ),
                "kernel/patch/module/module.c": "KP_EXPORT_SYMBOL(kpm_safe_unload_v1);\n",
            }
            for relative, content in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")

            imports = checker.parse_undefined_symbols(NM_OUTPUT)
            checker.verify_kpatch_exports(root, imports)
            (root / "kernel/patch/module/module.c").write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "not exported.*kpm_safe_unload_v1"):
                checker.verify_kpatch_exports(root, imports)


if __name__ == "__main__":
    unittest.main()
