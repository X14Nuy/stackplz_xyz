import pathlib
import unittest

from tools import gen_kpm_profiles as generator


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "kpm" / "platform" / "kpatch" / "runtime.c"
ASYNC = ROOT / "kpm" / "platform" / "kpatch" / "async.c"
PROFILES = ROOT / "kpm" / "profiles" / "profiles.json"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated function: {signature}")


class KPatchPlatformSafetyTests(unittest.TestCase):
    def setUp(self):
        self.source = RUNTIME.read_text(encoding="utf-8")

    def test_profile_memory_adapter_uses_nofault_copy(self):
        body = function_body(self.source, "static int spz_kpatch_read_memory")
        self.assertIn("spz_kpatch_nofault_read", body)
        self.assertNotIn("memcpy", body)

    def test_nofault_helper_is_resolved_before_profile_validation(self):
        body = function_body(self.source, "int spz_kpatch_runtime_prepare")
        resolve = body.index("profile->symbols.copy_from_kernel_nofault")
        validate = body.index("spz_profile_validate")
        self.assertLess(resolve, validate)

    def test_per_cpu_offsets_are_read_without_faulting(self):
        body = function_body(self.source, "int spz_kpatch_runtime_prepare")
        marker = "context->per_cpu_offsets[cpu]"
        offset_read = body.index(marker)
        nearby = body[max(0, offset_read - 300) : offset_read + 300]
        self.assertIn("spz_kpatch_nofault_read", nearby)
        self.assertNotIn("memcpy", nearby)

    def test_async_workqueue_pointer_uses_nofault_copy(self):
        source = ASYNC.read_text(encoding="utf-8")
        body = function_body(source, "int spz_kpatch_async_transport_init")
        self.assertIn("spz_kpatch_nofault_read", body)
        self.assertNotIn("memcpy(&context->system_unbound_wq", body)

    def test_memcpy_wrapper_calls_kpatch_function_pointer(self):
        profile = generator.load_profiles(PROFILES)[0]
        source = (ROOT / "kpm" / "platform" / "kpatch" / "builtins.c").read_text(
            encoding="utf-8"
        )
        memcpy_name = profile["kpatch"]["memcpy_kfunc"]
        memset_name = profile["kpatch"]["memset_kfunc"]
        if profile["kpatch"]["kfunc_exports_are_pointers"]:
            self.assertIn(f"extern void *(*{memcpy_name})", source)
            self.assertIn(f"extern void *(*{memset_name})", source)
            self.assertNotRegex(source, rf"extern void \*{memcpy_name}\s*\(")
            self.assertNotRegex(source, rf"extern void \*{memset_name}\s*\(")
        else:
            self.assertRegex(source, rf"extern void \*{memcpy_name}\s*\(")


if __name__ == "__main__":
    unittest.main()
