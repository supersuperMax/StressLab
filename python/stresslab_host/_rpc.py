"""JSON-RPC 2.0 in frames of the Language Server Protocol over two file descriptors.

A frame is ``Content-Length: <bytes>\\r\\n\\r\\n`` followed by the message as UTF-8 JSON
(docs/extensions/protocol.md). The application is on the other side.
"""

from __future__ import annotations

import json
import os
import queue
import threading
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import Any

#: Error codes of JSON-RPC 2.0 and of the protocol of modules.
PARSE_ERROR = -32700
INVALID_REQUEST = -32600
METHOD_NOT_FOUND = -32601
INVALID_PARAMS = -32602
INTERNAL_ERROR = -32603
COMMAND_FAILED = -32001
CANCELLED = -32002
CONNECTION_LOST = -32003
NOT_FOUND = -32004

MAX_PAYLOAD = 256 << 20
_MAX_HEADER = 4096


class ProtocolError(Exception):
    """The byte stream is not JSON-RPC in LSP frames."""


class RpcError(Exception):
    """A request was answered with an error."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.data = data


class ConnectionClosed(BaseException):
    """The application closed the connection: the host ends. A BaseException, so that the code
    of a module does not catch it by accident."""


def frame(message: dict[str, Any]) -> bytes:
    payload = json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False)
    body = payload.encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n\r\n" + body


class FrameReader:
    """Cuts a byte stream into payloads of frames."""

    def __init__(self) -> None:
        self._buffer = bytearray()
        self._length: int | None = None

    def feed(self, data: bytes) -> list[bytes]:
        self._buffer += data
        payloads: list[bytes] = []
        while True:
            if self._length is None:
                end = self._buffer.find(b"\r\n\r\n")
                if end < 0:
                    if len(self._buffer) > _MAX_HEADER:
                        raise ProtocolError("a frame header is too long or has no end")
                    break
                self._length = _content_length(bytes(self._buffer[:end]))
                del self._buffer[: end + 4]
            if len(self._buffer) < self._length:
                break
            payloads.append(bytes(self._buffer[: self._length]))
            del self._buffer[: self._length]
            self._length = None
        return payloads


def _content_length(headers: bytes) -> int:
    length: int | None = None
    for line in headers.split(b"\r\n"):
        name, colon, value = line.partition(b":")
        if not colon:
            raise ProtocolError(f"malformed header line {line!r}")
        if name.strip().lower() == b"content-length":
            text = value.strip()
            if not text.isdigit():
                raise ProtocolError(f"malformed Content-Length {text!r}")
            length = int(text)
    if length is None:
        raise ProtocolError("a frame without Content-Length")
    if length > MAX_PAYLOAD:
        raise ProtocolError(f"a frame of {length} bytes is too large")
    return length


@dataclass
class _Pending:
    event: threading.Event = field(default_factory=threading.Event)
    result: Any = None
    error: RpcError | None = None


class _Wake:
    """Put into the inbox when a response came, so the main thread looks at its requests."""


_WAKE = _Wake()
_CLOSED = object()


class Connection:
    """The host's side of the connection.

    A reader thread takes the messages of the application. Responses go to the requests waiting
    for them; requests and notifications go to the inbox of the main thread, except those that
    ``immediate`` handles on the reader thread (pings, cancelling of tasks). A request sent on the
    main thread waits by doing the inbox meanwhile (the application may call the module back while
    it waits); one sent on another thread just waits.
    """

    def __init__(
        self,
        input_fd: int,
        output_fd: int,
        immediate: Callable[[dict[str, Any]], bool] | None = None,
    ) -> None:
        self._input = input_fd
        self._output = output_fd
        #: Handles a message on the reader thread instead of the main thread if it returns True.
        self.immediate = immediate
        self._write_lock = threading.Lock()
        self._lock = threading.Lock()
        self._pending: dict[int, _Pending] = {}
        self._next_id = 1
        self._closed = False
        self.inbox: queue.Queue[Any] = queue.Queue()
        self._reader = threading.Thread(
            target=self._read, name="stresslab-host-reader", daemon=True
        )

    def start(self) -> None:
        self._reader.start()

    # --- Sending ---------------------------------------------------------------------------------

    def send(self, message: dict[str, Any]) -> None:
        message = {"jsonrpc": "2.0", **message}
        data = memoryview(frame(message))
        with self._write_lock:
            while data:
                try:
                    written = os.write(self._output, data)
                except OSError as error:
                    raise ConnectionClosed() from error
                data = data[written:]

    def notify(self, method: str, params: Any = None) -> None:
        message: dict[str, Any] = {"method": method}
        if params is not None:
            message["params"] = params
        self.send(message)

    def respond(self, id: Any, result: Any) -> None:
        self.send({"id": id, "result": result})

    def respond_error(self, id: Any, code: int, message: str, data: Any = None) -> None:
        error: dict[str, Any] = {"code": code, "message": message}
        if data is not None:
            error["data"] = data
        self.send({"id": id, "error": error})

    def request(
        self, method: str, params: Any = None, dispatch: Callable[[Any], None] | None = None
    ) -> Any:
        """Sends a request and returns its result; raises RpcError for an error answer.

        On the main thread ``dispatch`` does what comes in meanwhile.
        """
        pending = _Pending()
        with self._lock:
            if self._closed:
                raise ConnectionClosed()
            id = self._next_id
            self._next_id += 1
            self._pending[id] = pending
        message: dict[str, Any] = {"id": id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        if dispatch is not None and threading.current_thread() is threading.main_thread():
            while not pending.event.is_set():
                item = self.inbox.get()
                if item is _WAKE:
                    continue
                if item is _CLOSED:
                    self.inbox.put(_CLOSED)
                    raise ConnectionClosed()
                dispatch(item)
        else:
            pending.event.wait()
        if pending.error is not None:
            if pending.error.code == CONNECTION_LOST:
                raise ConnectionClosed()
            raise pending.error
        return pending.result

    # --- Receiving -------------------------------------------------------------------------------

    def next(self) -> Any:
        """The next request or notification for the main thread; raises ConnectionClosed at the
        end."""
        while True:
            item = self.inbox.get()
            if item is _WAKE:
                continue
            if item is _CLOSED:
                self.inbox.put(_CLOSED)
                raise ConnectionClosed()
            return item

    def _read(self) -> None:
        reader = FrameReader()
        try:
            while True:
                try:
                    data = os.read(self._input, 65536)
                except OSError:
                    break
                if not data:
                    break
                for payload in reader.feed(data):
                    self._take(payload)
        except ProtocolError:
            pass
        finally:
            self._close()

    def _take(self, payload: bytes) -> None:
        try:
            message = json.loads(payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return
        if not isinstance(message, dict):
            return
        if "method" not in message:
            self._response(message)
            return
        if self.immediate is not None and self.immediate(message):
            return
        self.inbox.put(message)

    def _response(self, message: dict[str, Any]) -> None:
        id = message.get("id")
        with self._lock:
            pending = self._pending.pop(id, None) if isinstance(id, int) else None
        if pending is None:
            return
        error = message.get("error")
        if isinstance(error, dict):
            pending.error = RpcError(
                int(error.get("code", INTERNAL_ERROR)),
                str(error.get("message", "")),
                error.get("data"),
            )
        else:
            pending.result = message.get("result")
        pending.event.set()
        self.inbox.put(_WAKE)

    def _close(self) -> None:
        with self._lock:
            self._closed = True
            pending = list(self._pending.values())
            self._pending.clear()
        for item in pending:
            item.error = RpcError(CONNECTION_LOST, "the application closed the connection")
            item.event.set()
        self.inbox.put(_CLOSED)
