from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class HostAnnotation:
    frame_line_no: int
    host_hint: str
    confidence: str | None = None
    host_decode: str | None = None
    validation_status: str | None = None
    validation_detail: str | None = None
    suppress_payload_hints: bool = False
    suppress_summary_hint: bool = False
    suppress_summary_payload_hint: bool = False
    summary_suffix: str | None = None
    extra_lines: list[str] = field(default_factory=list)
