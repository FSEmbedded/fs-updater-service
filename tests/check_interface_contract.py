#!/usr/bin/env python3
"""Fail the build when the published interface and the exported vtable disagree.

The XML is documentation: nothing parses it at runtime, so a method could be
exported without ever appearing there.

The file is also not well-formed XML, and predates this check being so: CLI
flags inside comments carry a double hyphen, which XML forbids there. Comments
are therefore stripped before parsing. That is a workaround, not an
endorsement -- a contract no tool can read is a contract nothing checks.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def documented_methods(xml_path: Path) -> set[str]:
    raw = xml_path.read_text()
    root = ET.fromstring(re.sub(r"<!--.*?-->", "", raw, flags=re.S))
    return {m.get("name") for m in root.iter("method")}


def exported_methods(src_path: Path) -> set[str]:
    return set(re.findall(r'SD_BUS_METHOD\(\s*"(\w+)"', src_path.read_text()))


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_interface_contract.py <interface.xml> <service.cpp>",
              file=sys.stderr)
        return 2

    xml_path, src_path = Path(sys.argv[1]), Path(sys.argv[2])
    documented = documented_methods(xml_path)
    exported = exported_methods(src_path)

    undocumented = sorted(exported - documented)
    phantom = sorted(documented - exported)

    if undocumented:
        print(f"exported but absent from the published interface: {undocumented}",
              file=sys.stderr)
    if phantom:
        print(f"published but not exported -- a caller would get UnknownMethod: {phantom}",
              file=sys.stderr)

    if undocumented or phantom:
        return 1

    print(f"interface contract consistent ({len(exported)} methods)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
