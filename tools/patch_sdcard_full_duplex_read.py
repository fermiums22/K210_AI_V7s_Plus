#!/usr/bin/env python3
"""Deprecated SD debug patch.

This script used to mutate lib/drivers/src/storage/sdcard.cpp before build.
That made tests non-repeatable and one version could hang K210 before KSD boot.
It is intentionally a no-op now.  Keep SD fixes in source commits instead.
"""

print("SDCARD_CMD_PROBE_PATCH_DISABLED_OK source_commits_only=1")
