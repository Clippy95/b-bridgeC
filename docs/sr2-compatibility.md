# Saints Row 2 compatibility

b-bridge forwards standard Direct3D 9 calls. There are no GTA IV engine hooks or
hard-coded GTA IV addresses in its rendering implementation. The distributed
`dist/GTAIV/dxvk.conf` is a GTA IV preset, however, and should not be treated as a
universal configuration. Other games need testing, especially device resets,
fullscreen/focus behavior, readbacks, and resource locking.

## Confirmed SR2 crash

Analyzed `SRPB_pc.exe.20261001132738.dmp`, using the retail
`SR2_pc.exe.i64` database (the renamed executable is a total conversion).

- Exception: `0xc0000005` in the installed client at `d3d9.dll + 0x553aa`.
- That instruction reads the front element of the surface's lock-info queue in
  `Direct3DSurface9_LSS::UnlockRect`.
- Caller: retail SR2 `Tex_register_callback_D19720`, return address `0x00d198b7`.
- SR2's cubemap path at `0x00d1989b` multiplies the current mip height by the
  original top-level byte-row size. It updates a different row-size variable for
  advancing the source pointer, but does not use it for `memcpy`.
- The failing surface at `0xa173e978` is 8x8 DXT1: pitch 16, CPU storage 32 bytes.
  SR2 copied 2,048 bytes into its shadow buffer at `0xa17f9510`.
- Neighboring queue allocations contain the same DXT1 bytes as the copied source;
  the queue's invalid front pointer is `0x73f273f3`. This is a CPU buffer overflow,
  before the bridge sends the mip upload to the server.

The D3D9 pitch for DXT formats counts bytes per row of compressed blocks:
[Microsoft's D3DLOCKED_RECT documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dlocked-rect).
SR2 assumes more writable storage than this tightly packed allocation provides.

## Compatibility setting

Use a matching rebuilt x86 client and x64 server and add this to `.trex/bridge.conf`:

```ini
client.padCubeTextureMipShadows = True
```

The option is off by default and does not inspect the executable name. It works
for `SR2_pc.exe`, `SRPB_pc.exe`, and other renamed copies without engine hooks.

It allocates each cubemap face/mip's CPU backing store to at least
`topLevelPitch * mipBlockRows`. The returned mip pitch, dimensions, and the amount
uploaded to the server stay unchanged. Both ordinary shadow memory and shared-heap
texture allocations use the padded size; shadow accounting includes the padding.
Only locked cubemap faces allocate this storage. Lower mips consume extra 32-bit
address space, so leave the option off for games with correct upload sizes. This
handles the observed legacy copy pattern, not arbitrary out-of-bounds writes.

Do not combine a rebuilt client with an older server: the bridge rejects differing
version strings, and protocol compatibility cannot be inferred from filenames.
The existing SR2 DXVK configuration and mods were preserved.

## Confirmed freeze after the cubemap fix

The user's next run passed the initial crashing point, then froze before the
character screen. Live snapshots were saved without terminating the game:

- `Z:\crashdump\SRPB-bbridge-hang-20261001.dmp`
- `Z:\crashdump\bridge64-bbridge-hang-20261001.dmp`
- `Z:\crashdump\SRPB-bbridge-hang-20261001-contexts.json` (WOW64 thread contexts)

The client's main thread was pushing into a full 100,000-entry device command
queue while holding the writer lock. A Juiced readback thread was waiting for
that lock in `GetRenderTargetData`. The server was pushing into its full
200-entry reply queue. The bridge logs had already reported failed resource
creation and readback waits.

`DFEngine.dll` is built from the Juiced repository. Its
`Monkey Patch/Render/bitmap.cpp::read_render_target` uses
`GetRenderTargetData` to copy a render target into a system-memory surface.
`Render/Render3D.cpp::ReadPlayerImage` also schedules a render-thread readback
and waits for a completion event. These readback calls expose the bridge's
response race during loading. The same failure was reproduced without either
Juiced or BlingGFX in the isolated test below, so disabling these mods is not
required to fix this bridge defect.

Previously, only command construction/enqueue was serialized. A thread released
the writer lock before waiting for and consuming its reply. Another thread could
then send a synchronous request and wait on the earlier thread's reply at the
front of the queue. Its UID check could exhaust the retry budget before that
earlier reply was consumed, leaving its own eventual reply undrained. The actual
reply-data reads and header pops also lacked consumer serialization. Inactive-window
zero-timeout waits made mismatched replies fail immediately rather than recover.

`Bridge::ResponseLock` now holds the existing recursive channel lock from before
sending each synchronous request until its response data and header are consumed.
It covers device/module queries, readbacks, and optional/create replies, even with
the non-synchronized device variant. Optional calls take this extra lock only when
the corresponding replies are enabled. Command publishing and state batching are
preserved; the fix changes no command format and needs no executable addresses.

## Runtime regression

`test/cubemap_mip_upload.cpp` uses a real D3D9 device and the bridge's 64-bit DXVK
server. It creates 16x16 and 512x512 DXT1, DXT5, and ARGB8 cubemaps, locks all six
faces and every mip, and checks the reported pitch and retained mip bytes.
It alternates surface and cubemap LockRect/UnlockRect entry points, tests null
surface lock output, and waits for a server-side GPU event before reporting success.

`--legacy` intentionally copies using the top-level pitch into every mip, reproducing
the oversized-copy pattern. Run it only with the padding option enabled.

Three tested configurations passed, with 270 uploads each:

1. Padding enabled, shadow memory, oversized copies.
2. Padding disabled, shadow memory, standard copies.
3. Padding enabled, shared heap for textures, oversized copies.

`--concurrent` runs two threads on one device: 2,000 vertex-buffer creations
alongside 1,000 render-target readbacks, verifying the expected pixel after each
readback. Before response serialization, this failed with `0x8876086c` for both
`CreateVertexBuffer` and `GetRenderTargetData`. After the fix, all three modes
passed, followed by 270 cubemap uploads in each run:

1. Create replies enabled, shadow memory, zero command timeout, standard copies.
2. All replies enabled, forced synchronized device, shadow memory, legacy copies.
3. Create replies disabled, forced non-synchronized device, shared textures,
   zero command timeout/infinite retries, legacy copies.

Both flags can be combined. The server still uses the existing vanilla DXVK DLL.

The server logs confirm device creation, command processing, and clean shutdown.
No failed D3D9 calls were reported. The vanilla-DXVK `QueryFeatureVersion` message
is expected because RTX extensions are absent.

Build the harness in an x86 Visual Studio developer prompt:

```bat
cl /nologo /EHsc /MT /O2 /W4 test\cubemap_mip_upload.cpp user32.lib /Fe:cubemap_mip_upload.exe
```

Place it in an isolated directory with the rebuilt `d3d9.dll` and a `.trex` directory
containing the matching server, `d3d9vk_x64.dll`, and `bridge.conf`. Run from that
directory so DXVK reads the test settings. The harness creates a hidden test window.

## Installed files and rollback

The tested files were installed into `Z:\Games\Saints Row 2`:

- `d3d9.dll`: x86 release client built with MSVC 14.44, `b_ndebug=true`, static CRT.
- `.trex/NvRemixBridge.exe`: matching x64 release server.
- `.trex/bridge.conf`: added `client.padCubeTextureMipShadows = True`.

The installed client/server SHA-256 hashes were checked against the tested build.
The synchronization update preserves the current config, including command/state
batching and the enabled cubemap padding option. Juiced and BlingGFX were not edited.

Original files are preserved in
`Z:\Games\Saints Row 2\modding\bbridge-backup-20261001-134541`.
The intermediate pair with only cubemap padding is also preserved in
`Z:\Games\Saints Row 2\modding\bbridge-before-response-fix-20261001-1408`.
To roll back, close the game/server and copy `d3d9.dll` to the game root and
`NvRemixBridge.exe` plus `bridge.conf` to `.trex` from that backup directory.

The user reported that the game seems to work after the response serialization
fix. Broader gameplay validation remains user-driven. The user controls all game
launches.

## FPS cap and DXVK GPLAsync

The installed bridge config had `clientFrameCap = 60`. At the user's request it is
now `clientFrameCap = 0`, which disables the bridge's frame pacer. Other game/driver
limits can still affect the achieved frame rate. This config change applies on the
next launch.

The previous server renderer was x64 upstream DXVK 3.0.2. The user requested
GPLAsync, so the x64 `d3d9.dll` from the official
[GPLAsync v3.1.1-1 release](https://gitlab.com/Ph42oN/dxvk-gplasync/-/releases/v3.1.1-1)
was installed as `.trex/d3d9vk_x64.dll`. The game root `d3d9.dll` remains the bridge
client. No bridge source change is needed to select this fork:
`server.useVanillaDxvk = True` loads the server DLL by that filename.

The game-root `dxvk.conf` already had `dxvk.enableAsync=true` and
`dxvk.enableGraphicsPipelineLibrary = Auto`. The isolated server log confirmed
`DXVK: v3.1.1-1-gplasync`, read that config, and reported graphics pipeline library
support on the Radeon RX 7900 XTX. GPLAsync's obsolete `dxvk.gplAsyncCache` option
was removed by its author in v2.7-1; that existing config line is not required.
See the [fork's README](https://gitlab.com/Ph42oN/dxvk-gplasync/-/raw/main/README.md).

Before installation, the new renderer passed the isolated regression: 2,000
concurrent buffer creations, 1,000 pixel-verified readbacks, and 270 legacy
cubemap uploads. This verifies the bridge interface, not full SR2 rendering with
this fork. Installed DLL SHA-256:
`644b722a8368897ec99ffc78e38c52ee1f3d468e0f208736e1ff555ba71651e0`.

The previous renderer and configs are backed up at
`Z:\Games\Saints Row 2\modding\bbridge-before-gplasync-20261001`.
Restore its `d3d9vk_x64.dll` to `.trex` to undo only the renderer change. Its
`bridge.conf` also restores the prior 60 FPS cap, and `dxvk.conf` preserves the
pre-change renderer settings.

