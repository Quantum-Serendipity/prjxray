# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC

# Suppress the following warnings;
# - env/lib/python3.7/distutils/__init__.py:4: DeprecationWarning: the imp module is deprecated in favour of importlib; see the module's documentation for alternative uses
export PYTHONWARNINGS=ignore::DeprecationWarning:distutils

# Make the prjxray repo root importable so the fuzzers can `import utils.*` and
# `import prjxray.*`. A modern (PEP 660) editable install only exposes the
# `prjxray` package, not the repo-root `utils/` directory, so add the root here.
XRAY_PYTHON_ROOT="${XRAY_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
export PYTHONPATH="${XRAY_PYTHON_ROOT}${PYTHONPATH:+:${PYTHONPATH}}"
