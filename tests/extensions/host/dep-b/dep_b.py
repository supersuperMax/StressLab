"""Reports the version of shared_dep it imports."""

from __future__ import annotations

import shared_dep


def activate(extension) -> None:
    extension.register_command("depb.version", lambda call: shared_dep.__version__)
