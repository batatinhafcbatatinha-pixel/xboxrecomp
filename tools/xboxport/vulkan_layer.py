"""Vulkan for any recompiled game, through the translation layers Proton uses (shared by the recompilers' port tools;
reference copy: port-installer/tools/vulkan_layer.py, each recompiler vendors it next to its port tool).

None of the runtimes has a Vulkan renderer of its own on Windows: the PSP and Xbox 360 ones draw with Direct3D 12, the
Xbox one with Direct3D 11. DXVK (Direct3D 8-11 and DXGI on Vulkan, zlib licence) and vkd3d-proton (Direct3D 12 on
Vulkan, LGPL 2.1) run those calls on Vulkan instead when their DLLs sit next to the game, as Windows loads a DLL from
the program's own folder first. stage() puts them there or takes them away again, and only when the machine has a
Vulkan device: otherwise the game keeps Direct3D, which is also where it goes back to if the layer cannot start.

    stage(exe_dir, api, want, cache_dir, log=print) -> bool   # True when the game will draw through Vulkan
    vulkan_devices() -> list of names                          # the Vulkan devices the loader sees
"""
import ctypes
import hashlib
import io
import os
import tarfile
import urllib.request
from pathlib import Path

# Pinned releases (hash of the archive).
DXVK = ('https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz',
        '40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608', 'dxvk-3.1.1/x64/')
VKD3D = ('https://github.com/HansKristian-Work/vkd3d-proton/releases/download/v3.1/vkd3d-proton-3.1.tar.zst',
         'b85ea8219b71093a3924913dd7aae6172333dc0c069983ed510816d67f02120b', 'vkd3d-proton-3.1/x64/')

# What each API needs next to the game. Direct3D 12 needs DXVK's DXGI too: vkd3d-proton presents through it.
FILES = {
    'd3d11': [(DXVK, 'dxgi.dll'), (DXVK, 'd3d11.dll')],
    'd3d12': [(DXVK, 'dxgi.dll'), (VKD3D, 'd3d12.dll'), (VKD3D, 'd3d12core.dll')],
}
MARKER = 'vulkan-layer.txt'   # the files stage() put there (and nothing else is ever removed)


def vulkan_devices():
    """Names of the Vulkan physical devices (empty without a loader, a driver or a device that supports Vulkan)."""
    if os.name != 'nt':
        return []
    try:
        vk = ctypes.WinDLL('vulkan-1.dll')
    except OSError:
        return []

    class InstanceInfo(ctypes.Structure):
        _fields_ = [('sType', ctypes.c_int), ('pNext', ctypes.c_void_p), ('flags', ctypes.c_uint32),
                    ('app', ctypes.c_void_p), ('layers', ctypes.c_uint32), ('layer_names', ctypes.c_void_p),
                    ('extensions', ctypes.c_uint32), ('extension_names', ctypes.c_void_p)]
    instance = ctypes.c_void_p()
    if vk.vkCreateInstance(ctypes.byref(InstanceInfo(1)), None, ctypes.byref(instance)) != 0:
        return []
    names = []
    try:
        count = ctypes.c_uint32(0)
        vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), None)
        devices = (ctypes.c_void_p * count.value)()
        if count.value and vk.vkEnumeratePhysicalDevices(instance, ctypes.byref(count), devices) in (0, 5):
            for device in devices[:count.value]:
                props = ctypes.create_string_buffer(1024)   # VkPhysicalDeviceProperties: the name is at offset 20
                vk.vkGetPhysicalDeviceProperties(ctypes.c_void_p(device), props)
                names.append(props.raw[20:276].split(b'\0', 1)[0].decode('utf-8', 'replace'))
    finally:
        vk.vkDestroyInstance(instance, None)
    return names


def _archive(release, cache_dir):
    url, sha, _ = release
    path = Path(cache_dir) / url.rsplit('/', 1)[1]
    if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != sha:
        path.parent.mkdir(parents=True, exist_ok=True)
        req = urllib.request.Request(url, headers={'User-Agent': 'port-tools'})
        with urllib.request.urlopen(req, timeout=120) as r:
            data = r.read()
        if hashlib.sha256(data).hexdigest() != sha:
            raise RuntimeError(f'{url}: unexpected download (hash differs)')
        path.write_bytes(data)
    return path


def _extract(release, name, cache_dir):
    """One DLL from a release, cached next to its archive."""
    out = Path(cache_dir) / release[2].strip('/').replace('/', '_') / name
    if out.is_file():
        return out
    archive = _archive(release, cache_dir)
    raw = archive.read_bytes()
    if archive.suffix == '.zst':
        from compression import zstd   # Python 3.14
        raw = zstd.decompress(raw)
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        member = tar.getmember(release[2] + name)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(tar.extractfile(member).read())
    return out


def _remove(exe_dir):
    marker = exe_dir / MARKER
    if not marker.is_file():
        return
    for name in marker.read_text(encoding='utf-8').split():
        try:
            (exe_dir / name).unlink()
        except OSError:
            pass
    marker.unlink()


def stage(exe_dir, api, want, cache_dir, log=print):
    """Vulkan for the game in exe_dir (api: 'd3d11' or 'd3d12') when want and the machine has a Vulkan device;
    otherwise removes what an earlier call put there. True when the game will draw through Vulkan."""
    exe_dir = Path(exe_dir)
    if not want or os.name != 'nt':
        _remove(exe_dir)
        return False
    devices = vulkan_devices()
    if not devices:
        _remove(exe_dir)
        log('Vulkan: no graphics card here supports Vulkan; the game keeps Direct3D.')
        return False
    marker = exe_dir / MARKER
    owned = set(marker.read_text(encoding='utf-8').split()) if marker.is_file() else set()
    try:
        wanted = {name for _, name in FILES[api]}
        for name in owned - wanted:   # the other API's files (the game's API changed)
            (exe_dir / name).unlink(missing_ok=True)
        owned &= wanted
        for release, name in FILES[api]:
            target = exe_dir / name
            if target.exists() and name not in owned:
                raise RuntimeError(f'{target} exists and is not the layer\'s; left as it is')
            data = _extract(release, name, cache_dir).read_bytes()
            owned.add(name)
            marker.write_text('\n'.join(sorted(owned)) + '\n', encoding='utf-8')   # before the file: always removable
            target.write_bytes(data)
    except Exception as e:   # noqa: BLE001 -- any failure leaves the game on Direct3D
        _remove(exe_dir)
        log(f'Vulkan: {e}; the game keeps Direct3D.')
        return False
    log(f'Vulkan: {api} through {"DXVK" if api == "d3d11" else "vkd3d-proton + DXVK"} on {devices[0]}.')
    return True


if __name__ == '__main__':
    print('Vulkan devices:', vulkan_devices() or 'none')
