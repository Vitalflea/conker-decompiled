# Image layout: default.xbe (Mar 14, 2005 prototype)

Ground truth file:
`iso/Conker Live & Reloaded (Mar 14, 2005 prototype)/default.xbe`
Size: 0x75E000 bytes (7,725,056).

## Physical layout

| Region | File offset | Notes |
|---|---|---|
| Prefix / header block | 0x000 – ~0x9xx | Starts `XBEH`; holds section-name list, import (IFSL-style) entries, and image metadata |
| Sections (.text … .XTLID) | 0x1000 – 0x75E000 | Contiguous; total image size 0x75D000 (stored at 0x92C) |

Key facts established by direct byte comparison (IDA memory vs retail file):

* IDA places every byte at **VA = physical offset + 0x1000** (verified contiguously over probed ranges; first live byte at VA 0x11000 = file 0x1000).
* `.text` therefore begins at VA 0x11000 / file 0x1000.
* Entry points per IDB: `start` @ 0x426AD0, `kernel_thunk` @ 0x558660.
* All non-prefix content matches the retail file exactly — no patches, no relocations in this build.

## Header fields decoded from the prefix

| Offset | Value | Meaning |
|---|---|---|
| 0x920 | 0x38 | count field (58) |
| 0x924 | 0x87F160 | VA of last section (.XTLID start) |
| 0x928 | 0xA28 | size of .XTLID |
| 0x92C | 0x75D000 | total image size (sections span 0x1000..0x75E000) |
| 0x934 | 0x10A50 | VA of end-of-header region |
| 0x984 | name list | `.text D3DX XNET XONLINE DSOUND XMV WMADEC XGRPH D3D PAGELK XACTENG EFFECT_U EFFECT_C EFFECTS_ XP P .rdata .data XON_RD .data1 ... $XTIMAGE $XSIMAGE` |
| 0xA60+ | 20-byte import entries | {u16 module_id, u32 size_in_file, u32 checksum, char[12] name}; ids seen: 0x0001–0x0009 for D3DX/XNET/XONLINE/DSOUND/XMV/WMADEC/D3D/PAGELK/XACTENG/EFFECT_* modules |

Section table (virtual addresses, from IDB):
```
.text     0x11000 - 0x48465C   (0x47365C)
D3DX    0x484660 - 0x484CA8   (0x648)
XNET    0x484CC0 - 0x497AA0   (0x12DE0)
XONLINE 0x497AA0 - 0x4BAD0C   (0x2326C)
DSOUND  0x4BAD20 - 0x4D9574   (0x1E854)
...     (remaining sections continue to .XTLID @ 0x87F160-0x87FB88)
```

## Import table (IFSL-style entries) — corrected readout

Entries start immediately after the section-name list (file offset ~0xA50..0xA60 region); each entry is **18 bytes**: `{u16 module_id, u32 size_in_file, char[12] name}` where names are padded to 12 chars (`D3DX`, `XONLINES`...). Decoded rows (module_id → name):

```
0x0001 D3DX        0x0001 XONLINE   0x0001 XVOICE      0x0001 VOICMAIL
0x0001 DSOUND     0x0001 XBOXKRNL  0x0001 XMV        0x0001 LIBCPMT
0x0001 LIBCMT    0x0001 D3D8LTCG  0x0001 XGRAPHCL  0x0001 XACTENLT
0x0001 VOICMAIL
```

The header also carries the build identity string:
`D:\work\Bfd\Game\BuildOutput\FinalRelease.exe` followed by a large binary blob (CRC/checksum data).
