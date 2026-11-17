import os

import pytest

# GitHub Actions sets CI=true. There a missing build output or tool is a failure, not a skip:
# a renamed ESPHome build path would otherwise turn the image checks off without anyone noticing.
IN_CI = os.environ.get("CI") == "true"


def require(ok, reason):
    """Skip locally, fail in CI, when a build output or tool a test needs is missing."""
    if ok:
        return
    if IN_CI:
        pytest.fail(f"{reason} (required in CI)")
    pytest.skip(reason)
