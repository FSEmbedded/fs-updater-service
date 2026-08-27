#!/usr/bin/env python3
"""Fail the build when the published interface and the exported vtable disagree.

The XML is documentation: nothing parses it at runtime, so a method could be
exported without ever appearing there. The file is parsed as plain XML with no
pre-processing, so a malformed contract (for example a double hyphen inside a
comment) fails here instead of being worked around.
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def documented_methods(xml_path: Path) -> set[str]:
    try:
        root = ET.parse(xml_path).getroot()
    except ET.ParseError as exc:
        # Say what is wrong with the contract rather than ending in a
        # traceback: this check is the only reader that would notice.
        raise SystemExit(
            f"the published interface is not well-formed XML: {xml_path}: {exc}\n"
            "a double hyphen inside a comment is the usual cause; write CLI "
            "flags without their leading dashes"
        )
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
