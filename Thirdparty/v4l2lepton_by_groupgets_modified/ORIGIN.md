# Origin and Modifications (v4l2lepton)

This directory contains a vendored and substantially modified derivative of `v4l2lepton`.

## Upstream
- Project: groupgets/LeptonModule
- Upstream path: software/v4l2lepton
- Upstream URL: https://github.com/groupgets/LeptonModule/tree/master/software/v4l2lepton
- Upstream license statement: GPLv3 (“Released under GPLv3”).
- Upstream snapshot (last code change in v4l2lepton dir):
  - Commit: 83f308f908f4ecead4e0b616b9062eaf922ff94d

## Vendoring into this repository
- Repo: But-Not-Yet/NIRHR_FIRLR_Fusion_SR
- Directory: Thirdparty/v4l2lepton_by_groupgets_modified
- Initial add / baseline commit in this repo:
  - Commit: 9383448046d40ea4d189d1587eb8a038661db7e4 (Feb 20, 2026)
- Subsequent notable fixes in this repo (within this directory):
  - “Major fix for packet receiving and reading segmentNumber” (9fa5790, Feb 21, 2026).

## Local modifications (high-level)
Compared to upstream `software/v4l2lepton`, this copy is significantly refactored to better support Lepton gen 3 and configurable output:

1) Build/layout changes
- Vendored the Lepton SDK build directory into this folder and updated Makefile paths:
  upstream uses `../raspberrypi_libs/leptonSDKEmb32PUB/...`,
  this repo uses `./leptonSDKEmb32PUB/...` and builds it in-place.

2) v4l2lepton core rewrite (functional changes)
- Added CLI options:
  `--type (2|3)`, `--out (rgb|y16)`, `--colormap (1|2|3)`, `--spi-mhz`, `--verbose`.
- Added Lepton 3 (160x120) support:
  segmentNumber handling, multi-segment buffering, and alignment logic for telemetry on/off.
- Added output format support:
  RGB24 and Y16 (16-bit grayscale) modes, with proper V4L2 format negotiation.
- Safety/robustness adjustments:
  colormap bounds note to avoid OOB access; improved reset/peek/stash logic.

(For contrast: upstream version is a simpler RGB24-only pipeline with fixed colormap and fixed 80x60 defaults.)

## How this directory is used in this repo (context)
This third-party derivative is intended to provide a thermal `/dev/video*` source (e.g., via v4l2loopback) for the repo’s dual-stream RTSP tool (`RTSP.py`), whose thermal branch is designed around a V4L2 input.

## License for this directory
Because this is a derivative of upstream `v4l2lepton` which is stated as GPLv3, this directory should be distributed under GPLv3 terms, retaining upstream notices.