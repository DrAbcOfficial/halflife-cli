"""In-game screenshot capture: wait for the engine's file, convert to PNG."""

import glob
import io
import os
import time

from game_process import IMAGE_EXTENSIONS

# The engine writes the screenshot file asynchronously; a candidate is only
# accepted once its size is stable across one poll interval.
SNAPSHOT_POLL_S = 0.5


def find_new_screenshot(before, directories, timeout, poll=SNAPSHOT_POLL_S):
    """Return the newest image file that appeared after `before` in any directory.

    `directories` is one path or an iterable of paths (the engine writes into
    the mod root on GoldSrc and into <mod>/screenshots on Sven Co-op). Raises
    TimeoutError when nothing appears in time.
    """
    if isinstance(directories, (str, os.PathLike)):
        directories = [directories]

    def scan():
        found = set()
        for directory in directories:
            found.update(
                os.path.abspath(p)
                for p in glob.glob(os.path.join(directory, "*"))
                if p.lower().endswith(IMAGE_EXTENSIONS)
            )
        return found

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        new = scan() - before
        if new:
            candidate = max(new, key=os.path.getmtime)
            size = os.path.getsize(candidate)
            time.sleep(poll)
            if os.path.getsize(candidate) == size:
                return candidate
        else:
            time.sleep(poll)
    raise TimeoutError("no new screenshot appeared within %ss" % timeout)


def shot_to_png(path, max_edge):
    """Decode a screenshot (BMP/TGA/...), optionally downscale.

    Returns (png, returned_w, returned_h, original_w, original_h); mouse
    coordinates use the original size.
    """
    from PIL import Image as PILImage  # imported lazily so pure tests skip Pillow

    with PILImage.open(path) as img:
        original_width, original_height = img.size
        img = img.convert("RGB")
        if max_edge and max(img.size) > max_edge:
            img.thumbnail((max_edge, max_edge))
        buf = io.BytesIO()
        img.save(buf, format="PNG")
        return buf.getvalue(), img.width, img.height, original_width, original_height
