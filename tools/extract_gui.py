#!/usr/bin/env python3
"""Package the GUI layer without gameplay, memory, driver, or reverse-engineering code.

The checked-in application keeps the GUI and host/game services in the same target.
This tool makes a source-only archive containing the UI implementation, ImGui
backend, UI assets, and the small public headers those files include. Feature
implementations and sensitive host-side modules are deliberately not copied.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

# Paths are relative to jni/. Keep this list explicit: a whitelist makes it much
# harder for a future gameplay/driver file to leak into the GUI deliverable.
FILES = [
    # Build files for the standalone GUI-only Android target.
    "Android.mk",
    "Application.mk",
    # Public UI contracts and state.
    "include/Android_draw/draw.h",
    "include/Android_touch/TouchHelperA.h",
    "include/ImGui/backends/imgui_impl_android.h",
    "include/ImGui/backends/imgui_impl_opengl3.h",
    "include/ImGui/backends/imgui_impl_opengl3_loader.h",
    "include/ImGui/Font/Font.h",
    "include/ImGui/imconfig.h",
    "include/ImGui/imgui.h",
    "include/ImGui/imgui_internal.h",
    "include/ImGui/imstb_rectpack.h",
    "include/ImGui/imstb_textedit.h",
    "include/ImGui/imstb_truetype.h",
    "include/VidAvatar.h",
    "include/anim_web.h",
    "include/app_state.h",
    "include/audio.h",
    "include/cfg.h",
    "include/config.h",
    "include/hud.h",
    "include/gui_only_input.h",
    "include/lang.h",
    "include/logfile.h",
    "include/main.h",
    "include/media/audio.h",
    "include/media/avatar_video.h",
    "include/media/icons.h",
    "include/menu.h",
    "include/native_surface/ANativeWindowCreator.h",
    "include/overlay_geometry.h",
    "include/stb_image/stb_image.h",
    "include/str.h",
    "include/theme.h",
    "include/ui_util.h",
    "include/viewmodel.h",
    "include/widgets.h",
    # GUI implementation and rendering support.
    "src/gui_only_main.cpp",
    "src/gui_only_stubs.cpp",
    "src/gui_only_input.cpp",
    "src/Android_draw/draw.cpp",
    "src/Blur/Blur.cpp",
    "src/Blur/Blur.h",
    "src/ImGui/backends/imgui_impl_android.cpp",
    "src/ImGui/backends/imgui_impl_opengl3.cpp",
    "src/ImGui/imgui.cpp",
    "src/ImGui/imgui_draw.cpp",
    "src/ImGui/imgui_tables.cpp",
    "src/ImGui/imgui_widgets.cpp",
    "src/VidAvatar.cpp",
    "src/audio.cpp",
    "src/config.cpp",
    "src/hud.cpp",
    "src/lang.cpp",
    "src/lang_tables.inc",
    "src/menu.cpp",
    "src/theme.cpp",
    "src/widgets.cpp",
    # Embedded GUI assets and the JPEG decoder used by the avatar widget.
    "src/media/avatar_video_data.inc",
    "src/third_party/tjpgd/tjpgd.c",
    "src/third_party/tjpgd/tjpgd.h",
    "src/third_party/tjpgd/tjpgdcnf.h",
    # UI-only compatibility declarations used by menu.cpp/hud.cpp. These are
    # headers only; their gameplay implementations are intentionally omitted.
    "include/aim.h",
    "include/esp_draw.h",
    "include/farm.h",
    "include/game.h",
    "include/process.h",
]

EXCLUDED = [
    "src/main.cpp",
    "src/aim.cpp",
    "src/silent_aim.cpp",
    "src/farm.cpp",
    "src/game.cpp",
    "src/game_farm.cpp",
    "src/game_markers.cpp",
    "src/game_viewmodel.cpp",
    "src/process.cpp",
    "src/esp_draw.cpp",
    "src/kernel_io.h",
    "src/mem_io.h",
    "src/metadata_dump.h",
    "src/mode_select.h",
    "src/stealth.h",
    "src/viewmodel_memory.h",
    "src/weapon_speed_memory.h",
    "src/weapon_tuning_memory.h",
    "src/Android_touch/TouchHelperA.cpp",  # touch injection is not GUI code
    "reversing/",
]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def build_archive(repo: Path, output: Path) -> None:
    jni = repo / "jni"
    missing = [p for p in FILES if not (jni / p).is_file()]
    if missing:
        raise SystemExit("Missing GUI source files:\n" + "\n".join(missing))

    output.parent.mkdir(parents=True, exist_ok=True)
    manifest = {
        "format": 1,
        "kind": "GUI source extraction",
        "source_commit": "unknown",
        "root": "jni",
        "files": [],
        "excluded": EXCLUDED,
        "note": (
            "The archive is a whitelist extraction of the GUI layer and its "
            "standalone GUI-only Android target. Gameplay, memory, kernel/driver, "
            "reverse-engineering, and touch injection implementations are not "
            "included. gui_only_stubs.cpp makes the host services inert for the "
            "GUI binary."
        ),
    }

    try:
        import subprocess
        manifest["source_commit"] = subprocess.check_output(
            ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True
        ).strip()
    except Exception:
        pass

    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rel in FILES:
            data = (jni / rel).read_bytes()
            archive_name = f"gui-source/jni/{rel}"
            zf.writestr(archive_name, data)
            manifest["files"].append({
                "path": archive_name,
                "bytes": len(data),
                "sha256": sha256(data),
            })
        zf.writestr(
            "gui-source/README.md",
            "# GUI-only source extraction\n\n"
            "This archive contains the GUI layer selected from the project: the "
            "menu, widgets, HUD, theme, configuration/language UI support, ImGui "
            "Android/OpenGL backends, embedded GUI assets, and the standalone "
            "GUI-only Android entry point.\n\n"
            "It intentionally does not contain gameplay implementations, process "
            "or memory access, kernel/driver transport, reverse-engineering "
            "material, or synthetic touch injection. gui_only_stubs.cpp keeps all "
            "non-GUI host services inert so the binary renders the interface only.\n",
        )
        zf.writestr(
            "gui-source/MANIFEST.json",
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build_archive(args.repo.resolve(), args.output.resolve())
    print(args.output.resolve())


if __name__ == "__main__":
    main()
