# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0
"""On-device test for claw-api.

Requires an esp32s3 with Wi-Fi reachability and a live OpenAI-compatible LLM
endpoint configured in `main/test_secrets.h` (copy from `test_secrets.h.example`
and fill in). This hits a real API and consumes tokens, so it is not a hermetic
CI test -- run it against hardware with credentials.
"""
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.generic
@pytest.mark.parametrize(
    'config',
    [
        'release',
    ],
    indirect=True,
)
@idf_parametrize('target', ['esp32s3'], indirect=['target'])
def test_claw_api(dut: Dut) -> None:
    dut.run_all_single_board_cases()
