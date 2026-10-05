"""Pydantic result models returned by the MCP tools."""

from pydantic import BaseModel


class GameStatus(BaseModel):
    running: bool
    pid: int | None = None
    mode: str  # "managed" (started by this server), "attached", or "none"
    host: str | None = None
    port: int | None = None
    protocol: str | None = None
    exit_code: int | None = None
    game_dir: str | None = None
    mod: str | None = None  # mod directory name, e.g. svencoop / cstrike


class ConsoleWindow(BaseModel):
    lines: list[str]
    next_cursor: int
    dropped: int = 0
    note: str | None = None


class UserMsgStatus(BaseModel):
    schema_file: str
    coord_size: int
    messages: int
    display: bool
    wrapped: int           # hooked on top of the game DLL's own hook
    self_registered: int   # no game DLL hook; monitor-created display-only entry
    pending: int           # not yet resolvable in the engine's usermsg list


class UserMsgEvent(BaseModel):
    seq: int               # monotonic event number, use as since_seq cursor
    name: str
    size: int
    detail: str            # decoded "field=value ..." part, raw hex for raw mode


class UserMsgEvents(BaseModel):
    events: list[UserMsgEvent]
    newest_seq: int        # highest seq recorded (0 when none); feed back as since_seq
    more_after: int | None = None  # matching events beyond this page
    note: str | None = None


class UserMsgMessages(BaseModel):
    schema_file: str
    extends: str | None
    coord_size: int
    messages: list[dict]   # {name, raw, note, fields: [{name, type, count, when, note, fields}]}
