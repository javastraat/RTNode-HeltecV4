"""Static checks on the captive-portal form. No board needed."""

import re
from pathlib import Path

PORTAL_SOURCE = Path(__file__).resolve().parent.parent / "FirewallConfig.h"


def _input_tag_source(source: str, field: str) -> str:
    # The tag is split across C string literals, so match up to its value attribute.
    match = re.search(rf"<input id='{field}'(.*?)value='", source, re.DOTALL)
    assert match, f"portal input {field} not found in {PORTAL_SOURCE.name}"
    return match.group(1)


def test_coordinate_inputs_offer_a_minus_key():
    # iOS's decimal and numeric keypads have no minus key, so a field that asks
    # for one cannot take a western or southern coordinate (issue #38).
    source = PORTAL_SOURCE.read_text()
    for field in ("advert_lat", "advert_lon"):
        tag = _input_tag_source(source, field)
        for keypad in ("decimal", "numeric"):
            assert f"inputmode='{keypad}'" not in tag, (
                f"{field} requests the {keypad} keypad, which has no minus key on iOS"
            )
