"""Focused-window checks, screenshot capture, and masking."""

import io
import json
import shutil
import subprocess


def focused_app_id():
    """The app-id of the focused window, or "" if it cannot be determined.

    niri only. On anything else this returns "" and the deny list therefore
    matches nothing - which is why the deny list is not the only gate, and why
    vision defaults to off.
    """
    try:
        out = subprocess.run(
            ["niri", "msg", "--json", "focused-window"],
            capture_output=True, timeout=2, text=True,
        )
        if out.returncode != 0:
            return ""
        window = json.loads(out.stdout)
        return (window.get("app_id") or "") if isinstance(window, dict) else ""
    except (OSError, ValueError, subprocess.SubprocessError):
        return ""


def capture(output, mask, longest=1280):
    """One screenshot of `output`, with `mask` painted out, as JPEG bytes.

    The mask is her: her outline and her speech bubble, in device pixels,
    computed by the daemon (see Shell::handleExt). Painting it out keeps her own
    last sentence from being handed back to the model as "what is on screen",
    which is how a character ends up answering itself.
    """
    shot = subprocess.run(
        ["grim", "-o", output, "-t", "png", "-"], capture_output=True, timeout=10
    )
    if shot.returncode != 0:
        raise IOError("grim: " + shot.stderr.decode("utf-8", "replace").strip())

    box = (mask["x"], mask["y"], mask["x"] + mask["w"], mask["y"] + mask["h"])
    try:
        from PIL import Image, ImageDraw

        image = Image.open(io.BytesIO(shot.stdout)).convert("RGB")
        ImageDraw.Draw(image).rectangle(box, fill=(20, 20, 24))
        image.thumbnail((longest, longest))
        out = io.BytesIO()
        image.save(out, "JPEG", quality=72)
        return out.getvalue()
    except ImportError:
        pass

    if not shutil.which("magick"):
        raise IOError("neither python3-pillow nor ImageMagick is installed,"
                      " so the picture cannot be masked - refusing to send it")
    converted = subprocess.run(
        ["magick", "png:-", "-fill", "#141418", "-draw", "rectangle %d,%d %d,%d" % box,
         "-resize", "%dx%d>" % (longest, longest), "-quality", "72", "jpg:-"],
        input=shot.stdout, capture_output=True, timeout=20,
    )
    if converted.returncode != 0:
        raise IOError("magick: " + converted.stderr.decode("utf-8", "replace").strip())
    return converted.stdout
