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


# D-Bus type codes for the argument types this interface actually uses. A type
# outside this map is a deliberate stop rather than a silent skip: a signature
# this check cannot spell is a signature it cannot compare.
_TYPE_CODES = {
    "u": "u", "b": "b", "i": "i", "s": "s", "t": "t", "d": "d",
    "y": "y", "n": "n", "q": "q", "x": "x", "o": "o", "g": "g",
}


def documented_signals(xml_path: Path) -> dict[str, str]:
    """Signal name -> signature, composed from the declared argument types."""
    root = ET.parse(xml_path).getroot()
    signals = {}
    for sig in root.iter("signal"):
        codes = []
        for arg in sig.iter("arg"):
            t = arg.get("type", "")
            if t not in _TYPE_CODES:
                raise SystemExit(
                    f"signal {sig.get('name')!r} declares argument type {t!r}, "
                    "which this check cannot compare -- extend _TYPE_CODES"
                )
            codes.append(_TYPE_CODES[t])
        signals[sig.get("name")] = "".join(codes)
    return signals


def exported_signals(src_path: Path) -> dict[str, str]:
    """Signal name -> signature, as registered in the vtable.

    Every SD_BUS_SIGNAL occurrence must be matched, not merely the ones whose
    signature this pattern happens to spell. A container type (a{sv}, (us)) or
    the named-argument macro would otherwise fail to match and drop the signal
    from the comparison entirely -- leaving an undocumented signal invisible to
    exactly the check meant to catch it. So the total is counted first and a
    shortfall is an error, not a quiet pass.
    """
    text = src_path.read_text()
    total = len(re.findall(r'SD_BUS_SIGNAL\w*\(', text))
    found = re.findall(r'SD_BUS_SIGNAL\(\s*"(\w+)"\s*,\s*"([a-z{}()]*)"', text)
    if len(found) != total:
        raise SystemExit(
            f"{total} SD_BUS_SIGNAL occurrences but only {len(found)} could be "
            "read; an unparsed one would be silently exempt from this check"
        )
    names = [n for n, _ in found]
    if len(set(names)) != len(names):
        raise SystemExit(f"duplicate signal registrations: {sorted(names)}")
    return dict(found)


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

    # Signals, and their signatures. Names alone are not enough here: a widened
    # signature is invisible to a caller that reads the leading fields and
    # fatal to a generated proxy that checks the whole thing, so the one change
    # the stability rules forbid is exactly the one a name-only check misses.
    doc_signals = documented_signals(xml_path)
    exp_signals = exported_signals(src_path)

    undocumented_sig = sorted(set(exp_signals) - set(doc_signals))
    phantom_sig = sorted(set(doc_signals) - set(exp_signals))
    mismatched = sorted(
        (name, doc_signals[name], exp_signals[name])
        for name in set(doc_signals) & set(exp_signals)
        if doc_signals[name] != exp_signals[name]
    )

    if undocumented_sig:
        print(f"emitted but absent from the published interface: {undocumented_sig}",
              file=sys.stderr)
    if phantom_sig:
        print(f"published but never emitted: {phantom_sig}", file=sys.stderr)
    for name, documented_sig, exported_sig in mismatched:
        print(f"signal {name}: the contract declares {documented_sig!r}, "
              f"the vtable exports {exported_sig!r} -- a caller built against "
              "the contract would reject or misread the message",
              file=sys.stderr)

    if undocumented or phantom or undocumented_sig or phantom_sig or mismatched:
        return 1

    print(f"interface contract consistent "
          f"({len(exported)} methods, {len(exp_signals)} signals with signatures)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
