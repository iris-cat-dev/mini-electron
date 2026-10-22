# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

import unittest

from actions import FACADE_APIS, check_directives


class CoffDirectivePolicyTest(unittest.TestCase):
    def test_sdk_x64_dispatch_preserves_crt_library_inventory(self):
        for alias in (
            '/alternatename:_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue',
            '/ALTERNATENAME:"_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue"',
        ):
            with self.subTest(alias=alias):
                self.assertEqual(
                    ["libcmt.lib", "libcpmt.lib"],
                    check_directives(
                        '/DEFAULTLIB:libcmt.lib /defaultlib:"libcpmt.lib" '
                        '/defaultlib:libcmt.lib ' + alias
                    ),
                )

    def test_unknown_or_modified_symbol_substitutions_are_rejected(self):
        for directive in (
            '/alternatename:MissingApi=FakeApi',
            '/alternatename:_Avx2WmemEnabled=OtherValue',
            '/alternatename:_Avx2WmemEnabled=_Avx2WmemEnabledWeakValueSuffix',
            '/alternatename:_Avx2WmemEnabledWeakValue=_Avx2WmemEnabled',
            '/alternatename:_AVX2WmemEnabled=_Avx2WmemEnabledWeakValue',
            '/alternatename:"_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue"suffix',
            '/alternatename:',
            '/alternatename:""',
            '/alternatename',
            '/wholearchive:old-engine.lib',
            '/WHOLEARCHIVE',
            '/force:unresolved',
            '/FORCE',
        ):
            with self.subTest(directive=directive):
                with self.assertRaises(RuntimeError):
                    check_directives(directive)

    def test_library_paths_and_unapproved_libraries_are_rejected(self):
        for directive in (
            r'/defaultlib:"C:\legacy\libcmt.lib"',
            '/defaultlib:../libcmt.lib',
            '/defaultlib:libssl.lib',
        ):
            with self.subTest(directive=directive):
                with self.assertRaises(RuntimeError):
                    check_directives(directive)


class FacadeOwnershipPolicyTest(unittest.TestCase):
    def test_real_vm_entrypoint_definitions_require_facade_provenance(self):
        for symbol in (
            "public: static class v8::Isolate * __cdecl v8::Isolate::New(struct v8::Isolate::CreateParams const &)",
            "public: static bool __cdecl v8::V8::Initialize(void)",
            "public: static void __cdecl v8::V8::InitializePlatform(class v8::Platform *)",
            "public: static class v8::MaybeLocal<class v8::Module> __cdecl v8::ScriptCompiler::CompileModule(class v8::Isolate *, class v8::ScriptCompiler::Source *)",
        ):
            with self.subTest(symbol=symbol):
                self.assertRegex(symbol, FACADE_APIS)

    def test_caller_parameter_and_return_types_are_not_vm_definitions(self):
        for symbol in (
            "public: static class v8::MaybeLocal<class v8::Module> __cdecl blink::V8ScriptRunner::CompileModule(class v8::Isolate *, class blink::ModuleScriptCreationParams const &, class WTF::TextPosition const &, enum v8::ScriptCompiler::CompileOptions, enum v8::ScriptCompiler::NoCacheReason, class blink::ReferrerScriptInfo const &)",
            "class v8::ScriptCompiler::Source * __cdecl blink::MakeSource(void)",
        ):
            with self.subTest(symbol=symbol):
                self.assertNotRegex(symbol, FACADE_APIS)


if __name__ == "__main__":
    unittest.main()
