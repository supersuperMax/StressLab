"""Named shared memory, as the application's ``fem::ipc::SharedMemory`` makes it.

Windows: a named file mapping (``mmap`` with a tag name). macOS and Linux: a POSIX shared memory
object; the name is given without the leading '/', which is added here.

``multiprocessing.shared_memory`` is not used: in Python 3.12 it registers every segment it opens
with the resource tracker (a separate process), which unlinks segments of the application when
the host ends (fixed by ``track=False`` only in 3.13).
"""

from __future__ import annotations

import mmap
import os
import secrets

if os.name != "nt":
    import _posixshmem  # The module behind multiprocessing.shared_memory.


def new_name() -> str:
    """A name nobody uses: short, because macOS allows 31 characters."""
    return f"slh{os.getpid()}_{secrets.token_hex(6)}"


def create(name: str, size: int) -> mmap.mmap:
    """A new segment of ``size`` bytes (at least one)."""
    size = max(size, 1)
    if os.name == "nt":
        return mmap.mmap(-1, size, tagname=name)
    fd = _posixshmem.shm_open("/" + name, os.O_CREAT | os.O_EXCL | os.O_RDWR, mode=0o600)
    try:
        os.ftruncate(fd, size)
        return mmap.mmap(fd, size)
    except BaseException:
        _posixshmem.shm_unlink("/" + name)
        raise
    finally:
        os.close(fd)


def attach(name: str, size: int) -> mmap.mmap:
    """Maps the first ``size`` bytes of a segment of the application."""
    size = max(size, 1)
    if os.name == "nt":
        return mmap.mmap(-1, size, tagname=name)
    fd = _posixshmem.shm_open("/" + name, os.O_RDWR, mode=0o600)
    try:
        if os.fstat(fd).st_size < size:
            raise ValueError(f"shared memory {name!r} is smaller than {size} bytes")
        return mmap.mmap(fd, size)
    finally:
        os.close(fd)


def unlink(name: str) -> None:
    """Removes the name of a segment this process created; the memory stays while mapped. On
    Windows the name goes away with the last mapping."""
    if os.name != "nt":
        try:
            _posixshmem.shm_unlink("/" + name)
        except FileNotFoundError:
            pass
