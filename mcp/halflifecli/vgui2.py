"""VGUI2 framing and client. No dependency on screenshots or OS input."""
import json
import time
import uuid

MAX_REPLY_BYTES = 32 * 1024
MAX_TEXT_BYTES = 64 * 1024
# RCON requests are limited to 510 bytes, including challenge/password. The
# chunk is hex inside JSON, then the JSON itself is hex, so account for 4x growth.
TEXT_CHUNK_BYTES = 32


def encode_request(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8").hex()


def decode_reply(output, request):
    if len(output.encode("utf-8")) > 1024 * 1024:
        raise ValueError("VGUI2 response too large")
    prefix = f"VGUI2 {request} "
    records = [line[len(prefix):].split() for line in output.splitlines() if line.startswith(prefix)]
    starts, ends, chunks = [], [], {}
    size = 0
    try:
        for record in records:
            if len(record) == 2 and record[0] in ("begin", "end"):
                (starts if record[0] == "begin" else ends).append(int(record[1]))
            elif len(record) == 3 and record[0] == "data":
                seq = int(record[1])
                if seq in chunks:
                    raise ValueError("duplicate VGUI2 chunk")
                chunks[seq] = bytes.fromhex(record[2])
                size += len(record[2])
                if size > MAX_REPLY_BYTES:
                    raise ValueError("VGUI2 response exceeds page budget")
            else:
                raise ValueError("malformed VGUI2 record")
        if len(starts) != 1 or starts != ends or not 0 < starts[0] <= 1024 or set(chunks) != set(range(starts[0])):
            raise ValueError("incomplete VGUI2 response; request a fresh page")
        value = json.loads(b"".join(chunks[i] for i in range(starts[0])).decode("utf-8"))
        if not isinstance(value, dict):
            raise ValueError("invalid VGUI2 response object")
        return value
    except (IndexError, UnicodeError, TypeError) as exc:
        raise ValueError("malformed VGUI2 response") from exc


def format_tree(page):
    lines = []
    for node in page.get("nodes", []):
        line = "  " * min(node.get("depth", 0), 64) + f'{node["ref"]} {node["class"]} ' + json.dumps(node["name"], ensure_ascii=False)
        for key in ("text", "bounds", "visible", "enabled", "selected", "value", "focused", "text_truncated"):
            if key in node:
                line += f" {key}=" + json.dumps(node[key], ensure_ascii=False, separators=(",", ":"))
        lines.append(line)
    if page.get("next_cursor"):
        lines.append(f'... next_cursor={page["next_cursor"]}')
    return "\n".join(lines)


class VGUI2Client:
    def __init__(self, manager):
        self.manager = manager

    def request(self, operation, args):
        # Arguments are hex-encoded JSON: never console syntax or executable text.
        request = uuid.uuid4().hex
        out = self.manager.run_command(f"cli.vgui2 {operation} --request {request} {encode_request(args)}", max_lines=-1)
        value = decode_reply(out, request)
        if value.get("error"):
            raise ValueError("VGUI2: " + value["error"])
        return value

    def tree(self, root=None, include_hidden=False, max_depth=None, cursor=None):
        if max_depth is not None and (type(max_depth) is not int or max_depth < 0):
            raise ValueError("max_depth must be a nonnegative integer")
        result = self.request("tree", dict(root=root, include_hidden=include_hidden, max_depth=max_depth, cursor=cursor))
        result["text_tree"] = format_tree(result)
        return result

    def inspect(self, ref, text_offset=0):
        if type(text_offset) is not int or text_offset < 0:
            raise ValueError("text_offset must be a nonnegative integer")
        return self.request("inspect", dict(ref=ref, text_offset=text_offset))

    def action(self, operation, **args):
        with self.manager._input_lock:
            result = self.request(operation, args)
            deadline = time.monotonic() + 5
            while result.get("status") == "pending":
                if time.monotonic() >= deadline:
                    raise ValueError("VGUI2 operation timed out; do not automatically repeat the action")
                time.sleep(0.025)
                result = self.request("result", {"operation": result["operation"]})
            return result

    def set_text(self, ref, text):
        raw = text.encode("utf-8")
        if len(raw) > MAX_TEXT_BYTES or "\0" in text:
            raise ValueError("VGUI2 text must be NUL-free and at most 64 KiB UTF-8")
        with self.manager._input_lock:
            token = self.request("set_text", dict(ref=ref, phase="begin", bytes=len(raw)))["transfer"]
            try:
                for offset in range(0, len(raw), TEXT_CHUNK_BYTES):
                    self.request("set_text", dict(phase="chunk", transfer=token, offset=offset,
                                                  data=raw[offset:offset + TEXT_CHUNK_BYTES].hex()))
                return self.action("set_text", phase="commit", transfer=token)
            except Exception:
                # Do not retry a commit: it may already have changed the control.
                try:
                    self.request("set_text", dict(phase="cancel", transfer=token))
                except Exception:
                    pass
                raise
