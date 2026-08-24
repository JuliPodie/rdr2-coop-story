# Local third-party verification

Verification date: 2026-07-28 (Europe/Berlin)

The following locally supplied files were scanned with Microsoft Defender
(real-time protection enabled, signatures `1.455.371.0`). No recent threat
detection was reported. These hashes document only the local copies; the
upstream author does not publish checksums.

| File | Bytes | SHA-256 |
|---|---:|---|
| `dinput8.dll` | 131072 | `956FB3765572D00F6C08BCAE11E9856A00A68107464A87B6CCC6C1FFED46B88A` |
| `ScriptHookRDR2.dll` | 185344 | `3AC29FBE8C92B664E358F7D4F0AF2EC9F1CA674885975087EF76BD98BF972A4C` |
| `NativeTrainer.asi` (must not be installed) | 401408 | `9B22CF7900E2DE297A013DFECE1F594203970A9CA4B03D7075753AEC7A602A80` |
| `ScriptHookRDR2.lib` | 6816 | `1A21C5547E9D0B8ACCD896C24F5D975150FBF31C9A5E376D75F7FB746FFF4E5A` |
| SDK `main.h` | 2174 | `C5BC5A0D1368928A009CC7183E1E4006664228E1F4DEA0A453360C350504A087` |
| SDK `natives.h` | 744265 | `735FE3D1DBD0092099F44E69C75C8A477FA00B28E4BBBDD8C42A76238BBCDAAD` |

The runtime and SDK readmes prohibit archive redistribution. The SDK may be
used only for scripts intended to work offline, and `ScriptHookRDR2.dll` must
not be bundled with this project. The meaning of “offline” for a private LAN
co-op experiment is not clarified by the author, so these files remain local
and any wider testing or distribution requires a separate permission/legal
review.

