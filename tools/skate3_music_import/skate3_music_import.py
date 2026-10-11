#!/usr/bin/env python3
"""Convenience entry point: python skate3_music_import.py <command> ..."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from s3mus.cli import main  # noqa: E402

sys.exit(main())
